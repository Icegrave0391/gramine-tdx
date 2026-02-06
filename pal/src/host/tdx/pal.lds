OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(pal_start);

/* Define segments (program headers) explicitly. Typically, linkers map sections to segments
 * themselves, and there is no need in PHDRS in the linker script. But in our case, we specify a
 * special layout of segments: code segment must be last. Note that only `PT_LOAD` kinds of segments
 * are loadable into VM memory and must not overlap; the other segments are contained inside the
 * `PT_LOAD` ones. */
PHDRS
{
  phdr PT_PHDR PHDRS;
  ro PT_LOAD FILEHDR PHDRS FLAGS(4);
  rw PT_LOAD FLAGS(6);
  rx PT_LOAD FLAGS(5);
  dyn PT_DYNAMIC;
  note PT_NOTE;
}

SECTIONS
{
  . = 0x0;

  PROVIDE_HIDDEN (__data_start = .);

  . += SIZEOF_HEADERS;

  /* Read-only (ro) segment */
  .note.gnu.build-id : { *(.note.gnu.build-id) } :ro :note
  .note              : { *(.note .note.*) } :ro :note
  .hash              : { *(.hash) } :ro
  .gnu.hash          : { *(.gnu.hash) } :ro
  .dynsym            : { *(.dynsym) } :ro
  .dynstr            : { *(.dynstr) } :ro
  .gnu.version       : { *(.gnu.version) } :ro
  .gnu.version_d     : { *(.gnu.version_d) } :ro
  .gnu.version_r     : { *(.gnu.version_r) } :ro
  .rela.plt          : { *(.rela.plt) *(.rela.iplt) } :ro
  .rodata            : { *(.rodata .rodata.* .gnu.linkonce.r.*) } :ro
  .eh_frame_hdr      : { *(.eh_frame_hdr) } :ro
  .eh_frame          : ONLY_IF_RO { *(.eh_frame) } :ro
  .rela.dyn          :
     {
      *(.rela.init)
      *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
      *(.rela.fini)
      *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
      *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
      *(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
      *(.rela.tbss .rela.tbss.* .rela.gnu.linkonce.tb.*)
      *(.rela.ctors)
      *(.rela.dtors)
      *(.rela.got)
      *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
      *(.rela.ldata .rela.ldata.* .rela.gnu.linkonce.l.*)
      *(.rela.lbss .rela.lbss.* .rela.gnu.linkonce.lb.*)
      *(.rela.lrodata .rela.lrodata.* .rela.gnu.linkonce.lr.*)
      *(.rela.ifunc)
    } :ro

  PROVIDE_HIDDEN (__ro_data_end = .);

  /* Read-writable (rw) segment */
  . = ALIGN(CONSTANT (MAXPAGESIZE));
  .data.rel.ro   : { *(.data.rel.ro) }
  .eh_frame      : ONLY_IF_RW { *(.eh_frame) } :rw
  .ctors         : { *(.ctors) } :rw
  .dtors         : { *(.dtors) } :rw
  .dynamic       : { *(.dynamic) } :rw :dyn
  .got           : { *(.got) *(.igot) } :rw
  .got.plt       : { *(.got.plt) *(.igot.plt) } :rw
  .init_array    : {
    __init_array_start = .;
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
    __init_array_end = .;
  } :rw
  .data          : { *(.data .data.*) } :rw

  /* Page-aligned (4KB) PAL serverless checkpoint data section */
  . = ALIGN(CONSTANT(COMMONPAGESIZE));
  __pal_serverless_data_start = .;
  .pal.serverless.data :
  {
    KEEP(*(.pal.serverless.data))
  } :rw
  __pal_serverless_data_end = .;
  . = ALIGN(CONSTANT(COMMONPAGESIZE));

  .bss           : { *(.bss .bss.*) *(COMMON) } :rw

  PROVIDE_HIDDEN (__data_end = .);

  /* Executable (rx) segment */
  . = ALIGN(CONSTANT (MAXPAGESIZE));
  PROVIDE_HIDDEN (__text_start = .);
  .init          : { *(.init) } :rx
  .fini          : { *(.fini) } :rx
  .plt           : { *(.plt) *(.iplt) } :rx
  .text          : { *(.text .stub .text.*) } :rx

  /* Page-aligned (4KB) PAL serverless checkpoint code section */
  . = ALIGN(CONSTANT(COMMONPAGESIZE));
  __pal_serverless_text_start = .;
  .pal.serverless.text :
  {
    KEEP(*(.pal.serverless.text))
  } :rx
  __pal_serverless_text_end = .;
  . = ALIGN(CONSTANT(COMMONPAGESIZE));

  PROVIDE_HIDDEN (__text_end = .);
}
