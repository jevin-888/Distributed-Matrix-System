// Copyright 2026 Distributed Matrix System
// Layout calculator public API

#pragma once

#include "common/Protocol.h"

#include <cstdint>
#include <vector>

namespace dms {

/**
 * @brief Calculates one protocol crop region per display node.
 *
 * The calculator uses the protocol layer's canonical ScreenLayout, VideoInfo,
 * and CropRegion structures so layout and command code do not maintain
 * duplicate public data contracts. The source video keeps its aspect ratio
 * and is center-cropped to the complete display wall.
 */
class LayoutCalculator {
public:
    LayoutCalculator() = default;
    ~LayoutCalculator() = default;

    /**
     * @brief Set the display-wall layout.
     * @throws std::invalid_argument when the layout is invalid.
     */
    void setLayout(const dms::ScreenLayout& layout);

    /**
     * @brief Set source video metadata.
     * @throws std::invalid_argument when width or height is zero.
     */
    void setVideoInfo(const dms::VideoInfo& video);

    /**
     * @brief Set explicit node placements on the complete display wall.
     * Empty placements retain the regular rows-by-cols mapping.
     */
    void setPlacements(const std::vector<dms::ScreenPlacement>& placements);

    /**
     * @brief Calculate protocol crop regions in ascending one-based node ID order.
     * @throws std::runtime_error when layout or video metadata is not set.
     */
    std::vector<dms::Protocol::CropRegion> calculateCropRegions() const;

    /**
     * @brief Return the protocol crop region for one node.
     * @throws std::out_of_range when nodeId is outside the configured wall.
     */
    dms::Protocol::CropRegion getCropRegion(uint32_t nodeId) const;

    const dms::ScreenLayout& getLayout() const { return layout_; }
    const dms::VideoInfo& getVideoInfo() const { return video_; }

private:
    static bool isLayoutValid(const dms::ScreenLayout& layout);
    static bool isVideoInfoValid(const dms::VideoInfo& video);
    void ensureConfigured() const;

    dms::ScreenLayout layout_{};
    dms::VideoInfo video_{};
    std::vector<dms::ScreenPlacement> placements_;
};

} // namespace dms
