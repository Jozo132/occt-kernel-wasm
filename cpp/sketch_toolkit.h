/**
 * @file sketch_toolkit.h
 * @brief Lightweight native sketch toolkit scaffold for the future 2D sketch solver.
 *
 * This target intentionally does not link OCCT. It exists so the repository can ship
 * a second WASM binary dedicated to fast-moving sketch solver work while the exact
 * OCCT modelling target continues to build separately.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace occt_kernel {

class SketchToolkitNative {
public:
    SketchToolkitNative();
    ~SketchToolkitNative() = default;

    SketchToolkitNative(const SketchToolkitNative&) = delete;
    SketchToolkitNative& operator=(const SketchToolkitNative&) = delete;

    std::string getSessionId() const;

    int32_t createSketch(const std::string& createParamsJson);
    void disposeSketch(int32_t sketchId);

    int32_t addEntity(int32_t sketchId, const std::string& entitySpecJson);
    void removeEntity(int32_t sketchId, int32_t entityId);

    int32_t addConstraint(int32_t sketchId, const std::string& constraintSpecJson);
    void removeConstraint(int32_t sketchId, int32_t constraintId);

    void setParameter(int32_t sketchId, const std::string& name, const std::string& valueJson);
    std::string solveSketch(int32_t sketchId, const std::string& optionsJson);

    std::string getSketchSnapshot(int32_t sketchId) const;
    std::string getLastError() const;
    void clearLastError();

private:
    struct StoredItem {
        int32_t id = 0;
        std::string json;
    };

    struct SketchRecord {
        int32_t id = 0;
        std::string createParamsJson;
        std::vector<StoredItem> entities;
        std::vector<StoredItem> constraints;
        std::unordered_map<std::string, std::string> parameters;
    };

    int32_t m_nextSketchId = 1;
    int32_t m_nextEntityId = 1;
    int32_t m_nextConstraintId = 1;
    std::string m_sessionId;
    mutable std::string m_lastError;
    std::unordered_map<int32_t, SketchRecord> m_sketches;

    SketchRecord& requireSketch(int32_t sketchId);
    const SketchRecord& requireSketch(int32_t sketchId) const;

    [[noreturn]] void throwError(const std::string& code, const std::string& detail) const;

    static std::string buildStoredItemsJson(const std::vector<StoredItem>& items);
    static std::string buildParameterMapJson(const std::unordered_map<std::string, std::string>& parameters);
    static std::string escapeJsonString(const std::string& value);
};

} // namespace occt_kernel
