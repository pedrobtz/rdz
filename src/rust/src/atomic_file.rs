use std::ffi::OsString;
use std::fs::{File, OpenOptions, Permissions};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::format::FormatError;

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

pub(crate) struct AtomicFile {
    destination: PathBuf,
    temporary: PathBuf,
    file: Option<File>,
    destination_permissions: Option<Permissions>,
    committed: bool,
}

impl AtomicFile {
    pub(crate) fn create(destination: &Path) -> Result<Self, FormatError> {
        if destination.is_dir() {
            return Err(FormatError::Invalid("destination is a directory"));
        }
        let destination_permissions = match std::fs::metadata(destination) {
            Ok(metadata) => Some(metadata.permissions()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
            Err(error) => return Err(error.into()),
        };
        let parent = destination.parent().unwrap_or_else(|| Path::new("."));
        let file_name = destination
            .file_name()
            .ok_or(FormatError::Invalid("destination has no file name"))?;

        for _ in 0..128 {
            let sequence = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
            let mut candidate_name = OsString::from(".");
            candidate_name.push(file_name);
            candidate_name.push(format!("-rdz-{}-{sequence}.tmp", std::process::id()));
            let temporary = parent.join(candidate_name);
            match OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
            {
                Ok(file) => {
                    return Ok(Self {
                        destination: destination.to_path_buf(),
                        temporary,
                        file: Some(file),
                        destination_permissions,
                        committed: false,
                    });
                }
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error.into()),
            }
        }

        Err(FormatError::Invalid(
            "could not create a unique temporary output file",
        ))
    }

    pub(crate) fn file_mut(&mut self) -> Result<&mut File, FormatError> {
        self.file
            .as_mut()
            .ok_or(FormatError::Invalid("output file is already closed"))
    }

    pub(crate) fn commit(mut self) -> Result<(), FormatError> {
        drop(self.file.take());
        if let Some(permissions) = self.destination_permissions.take() {
            std::fs::set_permissions(&self.temporary, permissions)?;
        }

        match std::fs::rename(&self.temporary, &self.destination) {
            Ok(()) => {
                self.committed = true;
                return Ok(());
            }
            Err(first_error) if !self.destination.exists() => return Err(first_error.into()),
            Err(_) => {}
        }

        let backup = backup_path(&self.destination)?;
        std::fs::rename(&self.destination, &backup)?;
        match std::fs::rename(&self.temporary, &self.destination) {
            Ok(()) => {
                self.committed = true;
                let _ = std::fs::remove_file(backup);
                Ok(())
            }
            Err(error) => {
                let _ = std::fs::rename(&backup, &self.destination);
                Err(error.into())
            }
        }
    }
}

impl Drop for AtomicFile {
    fn drop(&mut self) {
        if !self.committed {
            let _ = std::fs::remove_file(&self.temporary);
        }
    }
}

fn backup_path(destination: &Path) -> Result<PathBuf, FormatError> {
    let parent = destination.parent().unwrap_or_else(|| Path::new("."));
    let file_name = destination
        .file_name()
        .ok_or(FormatError::Invalid("destination has no file name"))?;

    for _ in 0..128 {
        let sequence = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let mut candidate_name = OsString::from(".");
        candidate_name.push(file_name);
        candidate_name.push(format!("-rdz-{sequence}.backup"));
        let candidate = parent.join(candidate_name);
        if !candidate.exists() {
            return Ok(candidate);
        }
    }

    Err(FormatError::Invalid(
        "could not create a unique replacement backup path",
    ))
}
