OUTPUT_FORMAT("elf64-littleaarch64", "elf64-bigaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

_start_phys = _start - %KERNEL_BASE% + %MEMBASE%;
ENTRY(_start_phys)

SECTIONS
{
    . = %KERNEL_BASE% + %KERNEL_LOAD_OFFSET%;

    /DISCARD/ : {
        *libclang_rt.builtins-aarch64-android.a:cpu_model.c.o(
            .text.init_cpu_features
            .text.init_have_lse_atomics
            .init_array*
        )
    }

    .text : AT(%MEMBASE% + %KERNEL_LOAD_OFFSET%) {
        __code_start = .;
        KEEP(*(.text.boot))
        KEEP(*(.text.boot.early.vectab))
        KEEP(*(.text.boot.vectab))
        *(.text* .sram.text.glue_7* .gnu.linkonce.t.*)
    }

    .interp : { *(.interp) }
    .hash : { *(.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .init : { *(.init) } =0x9090
    .plt : { *(.plt) }
    ASSERT(SIZEOF(.plt) == 0, ".plt section should be empty")

    __exidx_start = .;
    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) }
    __exidx_end = .;

    .fake_post_text : {
        __code_end = .;
    }

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        __fault_handler_table_start = .;
        KEEP(*(.rodata.fault_handler_table))
        __fault_handler_table_end = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    }

    .lk_init : {
        __lk_init = .;
        KEEP(*(.lk_init))
        __lk_init_end = .;
    }

    .apps : {
        __apps_start = .;
        KEEP(*(.apps))
        __apps_end = .;
    }

    .drivers : {
        __drivers = .;
        KEEP(*(.drivers))
        __drivers_end = .;
    }

    .rel.dyn : {
        *(.rel.text) *(.rel.gnu.linkonce.t*)
        *(.rel.init)
        *(.rel.plt)
        *(.rel.rodata) *(.rel.gnu.linkonce.r*)
        *(.rel.lk_init)
        *(.rel.apps)
        *(.rel.drivers)
        *(.rel.data) *(.rel.gnu.linkonce.d*)
        *(.rel.devices)
        *(.rel.ctors)
        *(.rel.dtors)
        *(.rel.got)
        *(.rel.bss) *(.rel.bss.*) *(.rel.gnu.linkonce.b*)
    }

    .rela.dyn : {
        *(.rela.text) *(.rela.gnu.linkonce.t*)
        *(.rela.init)
        *(.rela.plt)
        *(.rela.rodata) *(.rela.gnu.linkonce.r*)
        *(.rela.lk_init)
        *(.rela.apps)
        *(.rela.drivers)
        *(.rela.data) *(.rela.gnu.linkonce.d*)
        *(.rela.devices)
        *(.rela.ctors)
        *(.rela.dtors)
        *(.rela.got)
        *(.rela.bss) *(.rela.bss.*) *(.rela.gnu.linkonce.b*)
    }

    ASSERT(SIZEOF(.rel.dyn) == 0, "Found non-RELR relocations in .rel.dyn")
    ASSERT(SIZEOF(.rela.dyn) == 0, "Found non-RELR relocations in .rel.dyn")

    .relr.dyn : ALIGN(8) {
        __relr_start = .;
        *(.relr.dyn)
        __relr_end = .;
    }

    .ctors : ALIGN(8) {
        __ctor_list = .;
        KEEP(*(.ctors .init_array))
        __ctor_end = .;
    }
    .dtors : ALIGN(8) {
        __dtor_list = .;
        KEEP(*(.dtors .fini_array))
        __dtor_end = .;
    }

    .dynamic : { *(.dynamic) }
    .got : { *(.got.plt) *(.got) }

    .fake_post_rodata : {
        __rodata_end = .;
    }

    .data : ALIGN(4096) {
        __data_start_rom = .;
        __data_start = .;
        *(.data .data.* .gnu.linkonce.d.*)
    }

    .devices : {
        __devices = .;
        KEEP(*(.devices))
        __devices_end = .;
    }

    .fake_post_data : {
        __data_end = .;
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        KEEP(*(.bss.prebss.*))
        . = ALIGN(8);
        __post_prebss_bss_start = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(4096);
    _end = .;

    . = %KERNEL_BASE% + %MEMSIZE%;
    _end_of_ram = .;

    /DISCARD/ : { *(.comment .note .eh_frame) }
}
