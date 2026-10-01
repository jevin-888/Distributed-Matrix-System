// Copyright 2026 Distributed Matrix System
// Layout Calculator Implementation

#include "master/LayoutCalculator.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace dms {

bool LayoutCalculator::isLayoutValid(const dms::ScreenLayout& layout) {
    return layout.rows > 0 && layout.cols > 0 &&
           layout.width > 0 && layout.height > 0 &&
           layout.width <= 16384 && layout.height <= 16384 &&
           layout.rows <= 8 && layout.cols <= 8 &&
           static_cast<uint64_t>(layout.rows) * layout.cols <= dms::MAX_NODES;
}

bool LayoutCalculator::isVideoInfoValid(const dms::VideoInfo& video) {
    return video.width > 0 && video.height > 0 &&
           video.width <= 65535 && video.height <= 65535 && video.fps <= 240;
}

void LayoutCalculator::setLayout(const dms::ScreenLayout& layout) {
    if (!isLayoutValid(layout)) {
        throw std::invalid_argument(
            "Invalid layout: rows=" + std::to_string(layout.rows) +
            ", cols=" + std::to_string(layout.cols) +
            ", width=" + std::to_string(layout.width) +
            ", height=" + std::to_string(layout.height)
        );
    }
    layout_ = layout;
}

void LayoutCalculator::setVideoInfo(const dms::VideoInfo& video) {
    if (!isVideoInfoValid(video)) {
        throw std::invalid_argument(
            "Invalid video info: width=" + std::to_string(video.width) +
            ", height=" + std::to_string(video.height)
        );
    }
    video_ = video;
}

void LayoutCalculator::setPlacements(
    const std::vector<dms::ScreenPlacement>& placements) {
    if (placements.size() > dms::MAX_NODES) {
        throw std::invalid_argument("Too many screen placements");
    }

    for (const auto& placement : placements) {
        if (placement.nodeId == 0 ||
            !std::isfinite(placement.x) || !std::isfinite(placement.y) ||
            !std::isfinite(placement.width) || !std::isfinite(placement.height) ||
            placement.x < 0.0 || placement.y < 0.0 ||
            placement.width <= 0.0 || placement.height <= 0.0 ||
            placement.x + placement.width > 1.000001 ||
            placement.y + placement.height > 1.000001) {
            throw std::invalid_argument("Screen placement is invalid");
        }
    }
    placements_ = placements;
}

void LayoutCalculator::ensureConfigured() const {
    if (!isLayoutValid(layout_)) {
        throw std::runtime_error("Layout not configured");
    }
    if (!isVideoInfoValid(video_)) {
        throw std::runtime_error("Video info not configured");
    }
}

std::vector<dms::Protocol::CropRegion> LayoutCalculator::calculateCropRegions() const {
    ensureConfigured();

    const uint32_t totalNodes = layout_.rows * layout_.cols;
    std::vector<dms::Protocol::CropRegion> regions;
    regions.reserve(placements_.empty() ? totalNodes : placements_.size());

    const double videoAspect = static_cast<double>(video_.width) /
                               static_cast<double>(video_.height);
    const double totalScreenWidth = static_cast<double>(layout_.cols) * layout_.width;
    const double totalScreenHeight = static_cast<double>(layout_.rows) * layout_.height;
    const double screenAspect = totalScreenWidth / totalScreenHeight;

    uint32_t cropWidth = 0;
    uint32_t cropHeight = 0;
    uint32_t offsetX = 0;
    uint32_t offsetY = 0;

    if (videoAspect > screenAspect) {
        cropHeight = video_.height;
        cropWidth = static_cast<uint32_t>(std::round(cropHeight * screenAspect));
        offsetX = (video_.width - cropWidth) / 2;
    } else {
        cropWidth = video_.width;
        cropHeight = static_cast<uint32_t>(std::round(cropWidth / screenAspect));
        offsetY = (video_.height - cropHeight) / 2;
    }

    if (cropWidth < layout_.cols || cropHeight < layout_.rows) {
        throw std::invalid_argument("Source video is too small for the configured layout");
    }

    const auto appendRegion = [&](uint32_t nodeId, double x, double y,
                                  double width, double height) {
        const auto sourceCoordinate = [](uint32_t origin, uint32_t span,
                                         double normalized) {
            return origin + static_cast<uint32_t>(std::llround(span * normalized));
        };
        const uint32_t left = sourceCoordinate(offsetX, cropWidth, x);
        const uint32_t top = sourceCoordinate(offsetY, cropHeight, y);
        const uint32_t right = sourceCoordinate(offsetX, cropWidth, x + width);
        const uint32_t bottom = sourceCoordinate(offsetY, cropHeight, y + height);
        if (right <= left || bottom <= top || right > offsetX + cropWidth ||
            bottom > offsetY + cropHeight) {
            throw std::invalid_argument("Screen placement is too small after rounding");
        }
        regions.push_back({nodeId, left, top, right - left, bottom - top});
    };

    if (placements_.empty()) {
        for (uint32_t row = 0; row < layout_.rows; ++row) {
            for (uint32_t col = 0; col < layout_.cols; ++col) {
                const double x = static_cast<double>(col) / layout_.cols;
                const double y = static_cast<double>(row) / layout_.rows;
                appendRegion(row * layout_.cols + col + 1, x, y,
                             1.0 / layout_.cols, 1.0 / layout_.rows);
            }
        }
    } else {
        for (const auto& placement : placements_) {
            appendRegion(placement.nodeId, placement.x, placement.y,
                         placement.width, placement.height);
        }
    }

    return regions;
}

dms::Protocol::CropRegion LayoutCalculator::getCropRegion(uint32_t nodeId) const {
    ensureConfigured();

    if (!placements_.empty()) {
        for (const auto& placement : placements_) {
            if (placement.nodeId == nodeId) {
                LayoutCalculator calculator = *this;
                calculator.placements_ = {placement};
                return calculator.calculateCropRegions().front();
            }
        }
        throw std::out_of_range("Node " + std::to_string(nodeId) +
                                " has no configured screen placement");
    }

    const uint32_t totalNodes = layout_.rows * layout_.cols;
    if (nodeId == 0 || nodeId > totalNodes) {
        throw std::out_of_range(
            "Node ID " + std::to_string(nodeId) +
            " out of range [1, " + std::to_string(totalNodes) + "]"
        );
    }

    return calculateCropRegions()[nodeId - 1];
}

} // namespace dms
