import ctypes
import sys

def map_pe(file_path):
    with open(file_path, 'rb') as f:
        pe_data = f.read()

    e_lfanew = int.from_bytes(pe_data[0x3C:0x40], 'little')
    nt_headers = pe_data[e_lfanew:e_lfanew+256]
    size_of_image = int.from_bytes(nt_headers[0x50:0x54], 'little')
    buffer = ctypes.create_string_buffer(size_of_image)
    size_of_headers = int.from_bytes(nt_headers[0x54:0x58], 'little')
    ctypes.memmove(buffer, pe_data[:size_of_headers], size_of_headers)
    
    number_of_sections = int.from_bytes(nt_headers[0x06:0x08], 'little')
    size_of_optional_header = int.from_bytes(nt_headers[0x14:0x16], 'little')
    section_table_offset = e_lfanew + 24 + size_of_optional_header
    
    for i in range(number_of_sections):
        section = pe_data[section_table_offset + i*40 : section_table_offset + (i+1)*40]
        virtual_size = int.from_bytes(section[0x08:0x0C], 'little')
        virtual_address = int.from_bytes(section[0x0C:0x10], 'little')
        size_of_raw_data = int.from_bytes(section[0x10:0x14], 'little')
        pointer_to_raw_data = int.from_bytes(section[0x14:0x18], 'little')
        
        if size_of_raw_data > 0:
            ctypes.memmove(
                ctypes.addressof(buffer) + virtual_address,
                pe_data[pointer_to_raw_data:pointer_to_raw_data+size_of_raw_data],
                min(virtual_size, size_of_raw_data)
            )
    return buffer

buf = map_pe(sys.argv[1])
init_bytes = buf.raw[0x4b35f8:0x4b35f8+32]
trans_bytes = buf.raw[0x4b5538:0x4b5538+32]
print("Init 667:", init_bytes.hex())
print("Trans 667:", trans_bytes.hex())

