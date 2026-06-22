#!/usr/bin/env python3
"""Patch the guest's com_puc.c to register the SPCR-designated UART as console.
Reads /usr/src/sys/dev/puc/com_puc.c content on stdin, writes patched to stdout."""
import sys
src = sys.stdin.read()

# 1. includes: after pucvar.h include, add acpi (guarded) for SPCR console detect
inc_anchor = "#include <dev/pci/pucvar.h>\n"
inc_add = inc_anchor + (
    "\n"
    '#include "acpi.h"\n'
    "#if NACPI > 0\n"
    "#include <dev/acpi/acpireg.h>\n"
    "#include <dev/acpi/acpivar.h>\n"
    "#endif\n"
)
assert inc_anchor in src, "include anchor not found"
src = src.replace(inc_anchor, inc_add, 1)

# 2. helper: SPCR console detection (mirrors com_acpi_is_console), before com_puc_match
helper = (
    "#if NACPI > 0\n"
    "int\n"
    "com_puc_is_console(bus_addr_t addr)\n"
    "{\n"
    "\tstruct acpi_table_header *hdr;\n"
    "\tstruct acpi_spcr *spcr;\n"
    "\tstruct acpi_gas *base;\n"
    "\tstruct acpi_q *entry;\n"
    "\n"
    "\tif (acpi_softc == NULL)\n"
    "\t\treturn 0;\n"
    "\tSIMPLEQ_FOREACH(entry, &acpi_softc->sc_tables, q_next) {\n"
    "\t\thdr = entry->q_table;\n"
    "\t\tif (strncmp(hdr->signature, SPCR_SIG,\n"
    "\t\t    sizeof(hdr->signature)) == 0) {\n"
    "\t\t\tspcr = entry->q_table;\n"
    "\t\t\tbase = &spcr->base_address;\n"
    "\t\t\tif (base->address_space_id == GAS_SYSTEM_MEMORY &&\n"
    "\t\t\t    base->address == addr)\n"
    "\t\t\t\treturn 1;\n"
    "\t\t}\n"
    "\t}\n"
    "\treturn 0;\n"
    "}\n"
    "#endif\n"
    "\n"
)
match_anchor = "int\ncom_puc_match("
assert match_anchor in src, "match anchor not found"
src = src.replace(match_anchor, helper + match_anchor, 1)

# 3. attach hook: before com_attach_subr(sc), register console if SPCR-designated
attach_anchor = "\tcom_attach_subr(sc);\n"
attach_add = (
    "#if NACPI > 0\n"
    "\tif (com_puc_is_console(sc->sc_iobase) &&\n"
    "\t    comconsaddr != sc->sc_iobase) {\n"
    "\t\tcomcnattach(sc->sc_iot, sc->sc_iobase, B115200,\n"
    "\t\t    sc->sc_frequency,\n"
    "\t\t    (TTYDEF_CFLAG & ~(CSIZE | PARENB)) | CS8);\n"
    "\t\tprintf(\" (console)\");\n"
    "\t}\n"
    "#endif\n"
) + attach_anchor
assert attach_anchor in src, "attach anchor not found"
src = src.replace(attach_anchor, attach_add, 1)

sys.stdout.write(src)
