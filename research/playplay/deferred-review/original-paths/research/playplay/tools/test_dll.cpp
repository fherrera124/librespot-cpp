#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>

typedef void (*VmRuntimeInit)(void* vm_obj, void* vm_rt_context, int unk);
typedef void (*VmObjectTransform)(void* vm_obj, void* obf_key, void* derived_key, void* init_val);

std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t) strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

std::string bytes_to_hex(const uint8_t* bytes, size_t len) {
    std::string hex;
    for (size_t i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", bytes[i]);
        hex += buf;
    }
    return hex;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: test_dll <path_to_Spotify.dll>\n";
        return 1;
    }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        std::cerr << "Failed to load DLL\n";
        return 1;
    }
    
    uintptr_t base = (uintptr_t)dll;
    VmRuntimeInit vm_init = (VmRuntimeInit)(base + 0x4b35f8);
    VmObjectTransform vm_transform = (VmObjectTransform)(base + 0x4b5538);
    
    uint8_t vm_obj[144] = {0};
    uint8_t vm_rt_context[16] = {0};
    
    try {
        vm_init(vm_obj, vm_rt_context, 1);
        std::cout << "VM_RUNTIME_INIT completed without crash.\n";
    } catch (...) {
        std::cout << "VM_RUNTIME_INIT crashed!\n";
    }
    
    std::vector<uint8_t> obf = hex_to_bytes("628c9a976bedc0e3663bc9c4051f1708");
    std::vector<uint8_t> expected_aes = hex_to_bytes("a503a84c1dc9271460cc13f142e0bae2");
    std::vector<uint8_t> init_val = hex_to_bytes("8df84f8c610a1ab4c449a214fb08305e");
    uint8_t derived_key[24] = {0};
    
    try {
        vm_transform(vm_obj, obf.data(), derived_key, init_val.data());
        std::cout << "VM_OBJECT_TRANSFORM completed without crash.\n";
    } catch (...) {
        std::cout << "VM_OBJECT_TRANSFORM crashed!\n";
    }
    
    std::cout << "Derived key buf: " << bytes_to_hex(derived_key, 24) << "\n";
    
    // Search for AES in vm_obj
    bool found = false;
    for (size_t i = 0; i <= sizeof(vm_obj) - 16; ++i) {
        if (memcmp(vm_obj + i, expected_aes.data(), 16) == 0) {
            std::cout << "Found AES key in vm_obj at offset " << i << "\n";
            found = true;
        }
    }
    if (!found) std::cout << "AES key not found in vm_obj.\n";
    
    // Search in derived_key
    found = false;
    for (size_t i = 0; i <= sizeof(derived_key) - 16; ++i) {
        if (memcmp(derived_key + i, expected_aes.data(), 16) == 0) {
            std::cout << "Found AES key in derived_key at offset " << i << "\n";
            found = true;
        }
    }
    if (!found) std::cout << "AES key not found in derived_key.\n";
    
    return 0;
}
