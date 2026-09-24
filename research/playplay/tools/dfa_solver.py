import json

SBOX = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
]
INV_SBOX = [0]*256
for i in range(256): INV_SBOX[SBOX[i]] = i

def galois_mult(a, b):
    p = 0
    for _ in range(8):
        if b & 1: p ^= a
        hi_bit_set = a & 0x80
        a <<= 1
        if hi_bit_set: a ^= 0x1b
        b >>= 1
    return p % 256

# MixColumns multipliers for AES
# state column is multiplied by:
# 2 3 1 1
# 1 2 3 1
# 1 1 2 3
# 3 1 1 2
# When a fault `e` is injected in one byte before MixColumns, the difference diffuses:
# If fault is in byte 0: diff is (2e, e, e, 3e)
# If fault is in byte 1: diff is (3e, 2e, e, e)
# If fault is in byte 2: diff is (e, 3e, 2e, e)
# If fault is in byte 3: diff is (e, e, 3e, 2e)

# In ShiftRows:
# Row 0: 0, 4, 8, 12 (no shift)
# Row 1: 1, 5, 9, 13 (shift left 1) -> 5, 9, 13, 1
# Row 2: 2, 6, 10, 14 (shift left 2) -> 10, 14, 2, 6
# Row 3: 3, 7, 11, 15 (shift left 3) -> 15, 3, 7, 11

def get_column_indices(col_idx):
    if col_idx == 0: return [0, 13, 10, 7]
    if col_idx == 1: return [4, 1, 14, 11]
    if col_idx == 2: return [8, 5, 2, 15]
    if col_idx == 3: return [12, 9, 6, 3]

def get_mix_col_multipliers(row_idx):
    if row_idx == 0: return [2, 1, 1, 3]
    if row_idx == 1: return [3, 2, 1, 1]
    if row_idx == 2: return [1, 3, 2, 1]
    if row_idx == 3: return [1, 1, 3, 2]

# Load DFA data
with open("research/playplay/docs/dfa_sweep.json") as f: data = json.load(f)
correct = bytes.fromhex(data["correct"])

# Group faults by column
columns_faults = {0: [], 1: [], 2: [], 3: []}

for f in data["faults"]:
    faulty = bytes.fromhex(f["ct"])
    diff = [i for i in range(16) if correct[i] != faulty[i]]
    if len(diff) == 4:
        for c in range(4):
            if sorted(diff) == sorted(get_column_indices(c)):
                columns_faults[c].append(faulty)

print(f"Collected faults: Col0:{len(columns_faults[0])} Col1:{len(columns_faults[1])} Col2:{len(columns_faults[2])} Col3:{len(columns_faults[3])}")

recovered_k10 = [None]*16

for col in range(4):
    indices = get_column_indices(col)
    
    # We will find the 4 bytes of Round 10 key for these indices
    # We maintain a list of possible key bytes for this column.
    # Initial state: all 256^4 combinations are possible. Too slow.
    # We can guess `e` (the fault value) and one key byte, but wait: `e` is the SAME for the 4 bytes!
    
    possible_keys = None
    
    for faulty in columns_faults[col]:
        current_possible = set()
        
        # We don't know which row the fault was injected in, so we try all 4 rows
        for fault_row in range(4):
            mults = get_mix_col_multipliers(fault_row)
            
            for e in range(1, 256):
                # Calculate expected differences before SubBytes
                diff_in = [galois_mult(e, m) for m in mults]
                
                # Check if there exists a key tuple that satisfies this fault
                # For each byte in the column:
                valid_k = []
                for i in range(4):
                    idx = indices[i]
                    c_corr = correct[idx]
                    c_flt = faulty[idx]
                    d_in = diff_in[i]
                    
                    # We need SBOX_INV(c_corr ^ k) ^ SBOX_INV(c_flt ^ k) == d_in
                    valid_for_byte = []
                    for k in range(256):
                        if INV_SBOX[c_corr ^ k] ^ INV_SBOX[c_flt ^ k] == d_in:
                            valid_for_byte.append(k)
                    valid_k.append(valid_for_byte)
                
                # If all 4 bytes have at least one valid key, add the combinations
                if all(len(v) > 0 for v in valid_k):
                    for k0 in valid_k[0]:
                        for k1 in valid_k[1]:
                            for k2 in valid_k[2]:
                                for k3 in valid_k[3]:
                                    current_possible.add((k0, k1, k2, k3))
        
        if possible_keys is None:
            possible_keys = current_possible
        else:
            possible_keys = possible_keys.intersection(current_possible)
            
    print(f"Col {col} candidates:", len(possible_keys))
    if len(possible_keys) == 1:
        k_tuple = list(possible_keys)[0]
        for i in range(4):
            recovered_k10[indices[i]] = k_tuple[i]

if None in recovered_k10:
    print("Failed to fully recover K10")
else:
    k10_hex = bytes(recovered_k10).hex()
    print("Recovered K10:", k10_hex)
    print("Expected  K10:", "398cc5af7975a2ed0c547d6a005902e1")
