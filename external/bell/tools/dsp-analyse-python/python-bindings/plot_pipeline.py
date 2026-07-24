import sys
import numpy as np
import matplotlib.pyplot as plt
from scipy.fft import fft, fftfreq
from .dsp_wrapper import dsp_run_pipeline, dsp_parse_pipeline

def generate_stereo_noise(sample_rate, duration, pcm_min, pcm_max):
    """
    Generates stereo white noise in PCM format.
    """
    num_samples_per_channel = int(sample_rate * duration)

    # Generate noise for left and right channels
    left_channel_noise = np.random.randint(pcm_min, pcm_max + 1, num_samples_per_channel, dtype=np.int16)
    right_channel_noise = np.random.randint(pcm_min, pcm_max + 1, num_samples_per_channel, dtype=np.int16)
    # Interleave left and right channel noises for stereo
    stereo_noise = np.empty((num_samples_per_channel, 2), dtype=np.int16)
    stereo_noise = np.ravel(np.column_stack((left_channel_noise,right_channel_noise)))

    return stereo_noise

def process_dsp(input_pcm, output_pcm, num_channels, bit_width, sample_rate, pipeline_json_str):
    """
    Process the PCM data through a DSP pipeline.
    """
    dsp_parse_pipeline(pipeline_json_str)
    
    if dsp_run_pipeline(input_pcm, output_pcm, num_channels, bit_width, sample_rate):
        processed_noise = np.frombuffer(output_pcm, dtype=np.int16).reshape(-1, num_channels)
        return processed_noise
    else:
        raise RuntimeError("DSP pipeline processing failed")

def plot(stereo_noise, processed_noise, sample_rate, duration):
    """
    Plot the time-domain and frequency-domain representations for stereo data.
    """
    stereo_noise = stereo_noise.reshape(-1, 2)
    num_samples, channels = stereo_noise.shape
    frequencies = fftfreq(num_samples, 1 / sample_rate)
    
    # Compute FFT for each channel
    fft_original_left = fft(stereo_noise[:, 0])
    fft_original_right = fft(stereo_noise[:, 1])
    fft_processed_left = fft(processed_noise[:, 0])
    fft_processed_right = fft(processed_noise[:, 1])

    # Create a 4x2 grid of subplots
    fig, axs = plt.subplots(4, 2, figsize=(14, 12))
    
    # Time-domain plot: Original Left
    time = np.linspace(0, duration, num_samples, endpoint=False)
    axs[0, 0].plot(time, stereo_noise[:, 0], color="blue")
    axs[0, 0].set_title("Original Left Channel (Time Domain)")
    axs[0, 0].set_xlabel("Time (s)")
    axs[0, 0].set_ylabel("Amplitude")
    axs[0, 0].grid()

    # Time-domain plot: Original Right
    axs[0, 1].plot(time, stereo_noise[:, 1], color="red")
    axs[0, 1].set_title("Original Right Channel (Time Domain)")
    axs[0, 1].set_xlabel("Time (s)")
    axs[0, 1].set_ylabel("Amplitude")
    axs[0, 1].grid()

    # Frequency-domain plot: Original Left
    axs[1, 0].plot(frequencies[:num_samples // 2], np.abs(fft_original_left)[:num_samples // 2], color="blue")
    axs[1, 0].set_title("Original Left Channel (Frequency Domain)")
    axs[1, 0].set_xlabel("Frequency (Hz)")
    axs[1, 0].set_ylabel("Magnitude")
    axs[1, 0].grid()

    # Frequency-domain plot: Original Right
    axs[1, 1].plot(frequencies[:num_samples // 2], np.abs(fft_original_right)[:num_samples // 2], color="red")
    axs[1, 1].set_title("Original Right Channel (Frequency Domain)")
    axs[1, 1].set_xlabel("Frequency (Hz)")
    axs[1, 1].set_ylabel("Magnitude")
    axs[1, 1].grid()

    # Time-domain plot: Processed Left
    axs[2, 0].plot(time, processed_noise[:, 0], color="blue")
    axs[2, 0].set_title("Processed Left Channel (Time Domain)")
    axs[2, 0].set_xlabel("Time (s)")
    axs[2, 0].set_ylabel("Amplitude")
    axs[2, 0].grid()

    # Time-domain plot: Processed Right
    axs[2, 1].plot(time, processed_noise[:, 1], color="red")
    axs[2, 1].set_title("Processed Right Channel (Time Domain)")
    axs[2, 1].set_xlabel("Time (s)")
    axs[2, 1].set_ylabel("Amplitude")
    axs[2, 1].grid()

    # Frequency-domain plot: Processed Left
    axs[3, 0].plot(frequencies[:num_samples // 2], np.abs(fft_processed_left)[:num_samples // 2], color="blue")
    axs[3, 0].set_title("Processed Left Channel (Frequency Domain)")
    axs[3, 0].set_xlabel("Frequency (Hz)")
    axs[3, 0].set_ylabel("Magnitude")
    axs[3, 0].grid()

    # Frequency-domain plot: Processed Right
    axs[3, 1].plot(frequencies[:num_samples // 2], np.abs(fft_processed_right)[:num_samples // 2], color="red")
    axs[3, 1].set_title("Processed Right Channel (Frequency Domain)")
    axs[3, 1].set_xlabel("Frequency (Hz)")
    axs[3, 1].set_ylabel("Magnitude")
    axs[3, 1].grid()

    plt.tight_layout()
    plt.show()

def main():
    # Parse input arguments
    if len(sys.argv) != 2:
        print("Usage: python plot_pipeline.py [pipeline file json]")
        sys.exit(1)
    filename = sys.argv[1]
    # Read the pipeline JSON from the file
    with open(filename, 'r') as file:
        pipeline_json_str = file.read()
    # Configuration parameters
    SAMPLE_RATE = 44100  # Hz
    DURATION = 1.0  # seconds
    PCM_MAX = 32767  # Max value for uint16_t
    PCM_MIN = -32767  # Min value for uint16_t
    NUM_CHANNELS = 2  # Stereo
    BIT_WIDTH = 16  # 16-bit
    # Generate PCM stereo white noise
    stereo_noise = generate_stereo_noise(SAMPLE_RATE, DURATION, PCM_MIN, PCM_MAX)
    # Prepare input and output buffers
    input_pcm = stereo_noise.tobytes()
    output_pcm = bytearray(len(input_pcm))
    # Process through DSP pipeline
    processed_noise = process_dsp(input_pcm, output_pcm, NUM_CHANNELS, BIT_WIDTH, SAMPLE_RATE, pipeline_json_str)
    # Plot the results
    plot(stereo_noise, processed_noise, SAMPLE_RATE, DURATION)

if __name__ == "__main__":
    main()