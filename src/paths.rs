use alloc::{ffi::CString, format, string::String};
use reflex::fs;
pub unsafe fn beside_app(name: &[u8]) -> Result<CString, String> {
    let exec = unsafe { core::ffi::CStr::from_ptr((*crate::entry::CONTEXT).exec_path.cast()) };
    let metadata =
        unsafe { fs::metadata(exec) }.map_err(|code| format!("exec_path stat failed: {code}"))?;
    let mut path = exec.to_bytes().to_vec();
    if metadata.mode & 0xf000 != 0x4000 {
        let separator = path
            .iter()
            .rposition(|b| *b == b'\\' || *b == b'/')
            .ok_or_else(|| String::from("exec_path has no directory"))?;
        path.truncate(separator + 1);
    }
    if !matches!(path.last(), Some(b'\\' | b'/')) {
        path.push(b'\\');
    }
    path.extend_from_slice(name);
    Ok(CString::new(path).expect("exec_path contains no interior NUL"))
}
