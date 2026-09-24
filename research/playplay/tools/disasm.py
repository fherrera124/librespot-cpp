import capstone
import binascii

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def disasm(name, hex_str):
    print(f"--- {name} ---")
    for i in md.disasm(binascii.unhexlify(hex_str), 0):
        print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")

disasm("VM_RUNTIME_INIT", "40534883ec20488bd94585c07465488d052f7e3a01488901488d05a56b3a0148894118488d059a6b3a0148894128488d058f6b3a0148894138488d05ac6c3a01")
disasm("VM_OBJECT_TRANSFORM", "405356574154415541564157b8e0120000e8727f2501482be0488b05488b04024833c448898424d01200004c8bf24c8be148894c244848898c24800000004c89")
disasm("TRIGGER_RIP", "e88b7d29014569ccfb386707418bc44983c780241041c1e919f6d81bc981e1773f032481c17c3a5846442be140f6c7100f85f0000000498d7f5049bd2fbab5bf")
