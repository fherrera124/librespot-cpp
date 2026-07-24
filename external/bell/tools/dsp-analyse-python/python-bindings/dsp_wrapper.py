import ctypes, platform

# Load the shared library
# Note: The library file name may vary based on the platform
# For Windows, it may be 'build/lib/dsp_analyze.dll'
# For Linux, it may be 'build/lib/libdsp_analyze.so'
# For macOS, it may be 'build/lib/libdsp_analyze.dylib'
if platform.system() == 'Windows':
    lib = ctypes.CDLL('build/lib/dsp_analyze.dll')
elif platform.system() == 'Linux':
    lib = ctypes.CDLL('build/lib/libdsp_analyze.so')
elif platform.system() == 'Darwin':
  lib = ctypes.CDLL('build/lib/libdsp_analyze.dylib')

dsp_init = lib.init
dsp_init.argtypes = []
dsp_init.restype = None

dsp_init()

# Define the function signature
run_dsp_pipeline = lib.runDspPipeline
run_dsp_pipeline.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),  # const uint8_t* inputBuffer
    ctypes.c_size_t,                # size_t inputBufferLen
    ctypes.POINTER(ctypes.c_uint8), # uint8_t* outputBuffer
    ctypes.c_size_t,                # size_t outputBufferLen
    ctypes.c_uint8,                 # uint8_t numChannels
    ctypes.c_uint8,                 # uint8_t bitWidth
    ctypes.c_uint32,                # uint32_t sampleRate
]
run_dsp_pipeline.restype = ctypes.c_bool  # Return type: bool

parse_dsp_pipeline = lib.parseDspPipeline
parse_dsp_pipeline.argtypes = [
    ctypes.c_char_p,  # const char* jsonStr (string input)
    ctypes.c_size_t   # size_t jsonStrLen (length of the string)
]
parse_dsp_pipeline.restype = ctypes.c_bool  # Return type: bool

def dsp_parse_pipeline(json_str):
    # Convert the JSON string to a byte string
    json_bytes = json_str.encode('utf-8')

    # Get the length of the byte string
    json_len = len(json_bytes)

    # Call the function
    success = parse_dsp_pipeline(json_bytes, json_len)

    if success:
        print("Successfully parsed the DSP pipeline.")
    else:
        print("Failed to parse the DSP pipeline.")
        
# Example Usage
def dsp_run_pipeline(input_pcm, output_pcm, num_channels, bit_width, sample_rate):
    CHUNK_SIZE = 512  # Maximum buffer size for processing
    success = True
    # Create output buffer with the same size as input
    output_pcm.clear()
    output_pcm.extend(bytearray(len(input_pcm)))  # Ensure the output PCM is pre-allocated
    # Process PCM data in chunks
    for i in range(0, len(input_pcm), CHUNK_SIZE):
        # Determine the current chunk size
        chunk_size = min(CHUNK_SIZE, len(input_pcm) - i)
        # Prepare input and output buffers for the chunk
        input_data = (ctypes.c_uint8 * chunk_size).from_buffer_copy(input_pcm[i:i + chunk_size])
        output_data = (ctypes.c_uint8 * chunk_size)()
        
        # Call the DSP pipeline
        if not run_dsp_pipeline(
            input_data, chunk_size,
            output_data, chunk_size,
            num_channels, bit_width, sample_rate
        ):
            print(f"DSP pipeline failed at chunk {i // CHUNK_SIZE}.")
            success = False
            break
        # Copy processed chunk to the output PCM array
        output_pcm[i:i + chunk_size] = bytes(output_data)  # Ensure proper byte-level assignment
    if success:
        print("DSP pipeline processed all chunks successfully.")
    return success