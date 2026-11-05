core::arch::global_asm!(
    include_str!("../../brain_5th_hack/crates/reflex-macros/src/startup.S"),
    options(raw)
);
#[global_allocator]
static ALLOCATOR: reflex::allocator::SystemAllocator = reflex::allocator::SystemAllocator::new();
pub static mut CONTEXT: *const reflex::app::AppContext = core::ptr::null();

#[unsafe(no_mangle)]
unsafe extern "C" fn __reflex_main(context: *const reflex::app::AppContext) {
    unsafe {
        CONTEXT = context;
        reflex::lifecycle::start(context, crate::main, &ALLOCATOR);
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    reflex::lifecycle::panic(info)
}
