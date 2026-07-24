#include "bell/io/FileDataStream.h"

namespace bell::io {

FileDataStream::FileDataStream(std::ifstream&& file) : file(std::move(file)) {
  if (!this->file.is_open() || !this->file.good()) {
    throw std::runtime_error(
        "FileDataStream: provided ifstream is not open or in bad state");
  }

  // Determine size (store current pos, seek, then restore)
  auto currentPos = this->file.tellg();
  this->file.seekg(0, std::ios::end);
  std::streampos endPos = this->file.tellg();
  if (endPos >= 0) {
    fileSize = static_cast<size_t>(endPos);
  }
  this->file.seekg(currentPos, std::ios::beg);
}

size_t FileDataStream::position() const {
  auto pos = file.tellg();
  if (pos < 0) {
    return 0;
  }
  return static_cast<size_t>(pos);
}

bell::Result<> FileDataStream::seek(size_t offset, SeekOrigin origin) {
  auto stdOrigin = std::ios::beg;
  switch (origin) {
    case SeekOrigin::Begin:
      stdOrigin = std::ios::beg;
      break;
    case SeekOrigin::Current:
      stdOrigin = std::ios::cur;
      break;
    case SeekOrigin::End:
      stdOrigin = std::ios::end;
      offset = 0 - offset;
      break;
  }

  file.seekg(static_cast<std::streamoff>(offset), stdOrigin);
  if (!file) {
    return bell::make_unexpected_errc(std::errc::bad_file_descriptor);
  }
  return {};
}

bell::Result<size_t> FileDataStream::read(std::byte* outputBuffer,
                                          size_t outputBufferLen) {
  if (!file) {
    return bell::make_unexpected_errc<size_t>(std::errc::bad_file_descriptor);
  }

  if (outputBufferLen == 0) {
    return 0;  // Nothing to read
  }

  if (file.eof() || (position() == fileSize)) {
    return 0;  // End of file reached
  }

  file.read(reinterpret_cast<char*>(outputBuffer),
            static_cast<std::streamsize>(outputBufferLen));
  std::streamsize bytesRead = file.gcount();

  // Check if the read operation was successful
  if (file.bad()) {
    return bell::make_unexpected_errc<size_t>(std::errc::io_error);
  }

  return bytesRead;
}

}  // namespace bell::io
