import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
code = bytes.fromhex("40534883ec20488bd94585c07465488d052f7e3a01488901488d05a56b3a0148894118488d059a6b3a0148894128488d058f6b3a0148894138488d05ac6c3a01")
for i in md.disasm(code, 0x1803E42AC):
    print("0x%x:\t%s\t%s" %(i.address, i.mnemonic, i.op_str))
