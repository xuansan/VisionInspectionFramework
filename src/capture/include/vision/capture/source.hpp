#pragma once
#include <vision/contracts/frame.hpp>
#include <filesystem>
#include <span>

namespace vision::capture {
inline constexpr std::uint64_t max_frame_bytes=16*1024*1024;
inline constexpr std::size_t max_header_bytes=4096;
enum class SourceKind { Synthetic, Fixed, Sequence };
struct SourceConfig {
    SourceKind kind{SourceKind::Synthetic};
    std::uint32_t width{64},height{48},channels{1},seed{1};
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;
};
struct Image {
    contracts::ImageLayout layout;
    std::vector<std::byte> pixels;
};
// Bounded, synchronous I/O: call only on the camera worker thread.
class Source {
public:
    explicit Source(SourceConfig config);
    Image read(std::uint64_t sequence) const;
    contracts::ImageLayout layout() const;
private:
    SourceConfig config_;
    std::filesystem::path root_;
};
// Strict 8-bit P5/P6 subset; header comments and CRLF are accepted.
Image decode_pnm(std::span<const std::byte> bytes,contracts::ImageLayout expected);
Image decode_pnm(std::span<const std::byte> bytes);
} // namespace vision::capture
