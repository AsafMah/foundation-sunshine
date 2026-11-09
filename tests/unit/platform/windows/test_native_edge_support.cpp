/**
 * @file tests/unit/platform/windows/test_native_edge_support.cpp
 * @brief The Win32 edge's conversions: ContainerId bytes, arrival time, rotation and
 *        parent-hardware-ID classification.
 */
#include <gtest/gtest.h>

#include "src/platform/windows/display_device/native_edge.h"

namespace {
  using namespace display_device::topology;
  using namespace display_device::topology::win32_edge;
  using namespace std::string_literals;

  TEST(Win32NativeEdge, ContainerIdBytesAreInTextualOrderAndMatchTheCreateGuid) {
    // {01234567-89AB-CDEF-0123-456789ABCDEF} as Windows lays it out in memory.
    constexpr GUID guid { 0x01234567, 0x89ab, 0xcdef, { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef } };
    const auto bytes = container_bytes(guid);
    EXPECT_EQ(bytes, (uuid_bytes_t { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }));
    EXPECT_EQ(format_guid(bytes), "{01234567-89AB-CDEF-0123-456789ABCDEF}");

    constexpr GUID asymmetric { 0xa1b2c3d4, 0xe5f6, 0x0718, { 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90 } };
    EXPECT_EQ(format_guid(container_bytes(asymmetric)), "{A1B2C3D4-E5F6-0718-293A-4B5C6D7E8F90}");
    EXPECT_EQ(parse_guid("{a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90}"), container_bytes(asymmetric));
  }

  TEST(Win32NativeEdge, ArrivalIsTheFullFiletime) {
    EXPECT_EQ(filetime_ticks(FILETIME { 0x89abcdef, 0x01234567 }), 0x0123456789abcdefull);
    EXPECT_EQ(filetime_ticks(FILETIME { 0, 0 }), 0u);
    EXPECT_EQ(filetime_ticks(FILETIME { 0xffffffff, 0 }), 0xffffffffull);
  }

  TEST(Win32NativeEdge, RotationMapsOnlyTheFourOrientations) {
    EXPECT_EQ(rotation_from(DISPLAYCONFIG_ROTATION_IDENTITY), rotation_e::identity);
    EXPECT_EQ(rotation_from(DISPLAYCONFIG_ROTATION_ROTATE90), rotation_e::clockwise90);
    EXPECT_EQ(rotation_from(DISPLAYCONFIG_ROTATION_ROTATE180), rotation_e::clockwise180);
    EXPECT_EQ(rotation_from(DISPLAYCONFIG_ROTATION_ROTATE270), rotation_e::clockwise270);
    EXPECT_EQ(rotation_from(static_cast<DISPLAYCONFIG_ROTATION>(0)), std::nullopt);
    EXPECT_EQ(rotation_from(DISPLAYCONFIG_ROTATION_FORCE_UINT32), std::nullopt);
  }

  TEST(Win32NativeEdge, OnlyAZakoVddParentMakesADriverHead) {
    EXPECT_EQ(classify_parent_hardware_ids(std::nullopt), head_class_e::unknown);
    EXPECT_EQ(classify_parent_hardware_ids(L"Root\\ZakoVDD\0ZakoVDD\0"s), head_class_e::driver_head);
    EXPECT_EQ(classify_parent_hardware_ids(L"ZakoVDD\0root\\zakovdd\0"s), head_class_e::driver_head);
    EXPECT_EQ(classify_parent_hardware_ids(L"PCI\\VEN_10DE&DEV_2684\0PCI\\VEN_10DE\0"s), head_class_e::physical);
    EXPECT_EQ(classify_parent_hardware_ids(L"ZakoVDD\0Root\\ZakoVDD2\0Root\\ZakoVD\0"s), head_class_e::physical)
      << "only the exact root hardware ID names the driver";
    EXPECT_EQ(classify_parent_hardware_ids(L""s), head_class_e::physical);
    EXPECT_EQ(classify_parent_hardware_ids(L"Root\\ZakoVDD"s), head_class_e::driver_head) << "an unterminated last entry still counts";
    EXPECT_EQ(classify_parent_hardware_ids(L"PCI\\X\0\0Root\\ZakoVDD\0"s), head_class_e::physical)
      << "entries after the empty terminator are not part of the list";
  }

  TEST(Win32NativeEdge, AnotherVirtualDisplayDriverIsNotAZakoVddHead) {
    // Punktfunk Virtual Display, observed on a host next to ZakoVDD.
    EXPECT_EQ(classify_parent_hardware_ids(L"root\\pf_vdisplay\0"s), head_class_e::physical);
    EXPECT_EQ(classify_parent_hardware_ids(L"ROOT\\PF_VDISPLAY\0pf_vdisplay\0"s), head_class_e::physical);
  }

  // --- arrange planning against strict CCD rules (gh-all run 20261109-174516: ERROR_INVALID_PARAMETER) ---

  struct ccd_t {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  };

  /** One active path with its own source and target modes, as QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS | VIRTUAL_MODE_AWARE). */
  void
  add_active(ccd_t &ccd, LONG adapter, UINT32 target, LONG x, LONG y, UINT32 w, UINT32 h) {
    const LUID luid { static_cast<DWORD>(adapter), 0 };
    DISPLAYCONFIG_MODE_INFO source {};
    source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    source.id = 0;
    source.adapterId = luid;
    source.sourceMode.width = w;
    source.sourceMode.height = h;
    source.sourceMode.position = POINTL { x, y };
    DISPLAYCONFIG_MODE_INFO target_mode {};
    target_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    target_mode.id = target;
    target_mode.adapterId = luid;
    DISPLAYCONFIG_PATH_INFO path {};
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.adapterId = luid;
    path.sourceInfo.id = 0;
    path.sourceInfo.sourceModeInfoIdx = static_cast<UINT32>(ccd.modes.size());
    path.sourceInfo.cloneGroupId = 0;
    path.targetInfo.adapterId = luid;
    path.targetInfo.id = target;
    path.targetInfo.targetModeInfoIdx = static_cast<UINT32>(ccd.modes.size() + 1);
    path.targetInfo.desktopModeInfoIdx = 0xffff;
    path.targetInfo.targetAvailable = TRUE;
    ccd.modes.push_back(source);
    ccd.modes.push_back(target_mode);
    ccd.paths.push_back(path);
  }

  /** The Punktfunk display (adapter 3, target 260) and, active but unidentifiable, the ZakoVDD head (adapter 9, target 256). */
  std::string
  run_5_identify(const DISPLAYCONFIG_PATH_INFO &path) {
    return path.targetInfo.id == 260 ? "{712f4751-2e65-5e95-9578-a4e3b69ce687}" : "";
  }

  TEST(Win32NativeEdge, StrictCcdRulesCatchOverlapsAndBadModeIndices) {
    ccd_t ccd;
    add_active(ccd, 3, 260, 0, 0, 2560, 1440);
    add_active(ccd, 9, 256, 2560, 0, 1920, 1080);
    EXPECT_EQ(ccd_problem(ccd.paths, ccd.modes), std::nullopt);
    auto overlap = ccd;
    overlap.modes[2].sourceMode.position = POINTL { 1000, 0 };
    EXPECT_TRUE(ccd_problem(overlap.paths, overlap.modes).has_value());
    auto out_of_range = ccd;
    out_of_range.paths[1].sourceInfo.sourceModeInfoIdx = 9;
    EXPECT_TRUE(ccd_problem(out_of_range.paths, out_of_range.modes).has_value());
    auto wrong_type = ccd;
    wrong_type.paths[1].sourceInfo.sourceModeInfoIdx = 3;
    EXPECT_TRUE(ccd_problem(wrong_type.paths, wrong_type.modes).has_value());
    auto wrong_target = ccd;
    wrong_target.paths[0].targetInfo.targetModeInfoIdx = 0;
    EXPECT_TRUE(ccd_problem(wrong_target.paths, wrong_target.modes).has_value());
  }

  TEST(Win32NativeEdge, ArrangeNeverSendsAnActivePathTheInventoryCannotIdentify) {
    // Run 5: the head's monitor devnode was disabled, so its still-active path had no device path and the inventory
    // could not see it; Punktfunk had been moved to 7680,0. Putting Punktfunk back at 0,0 sent both paths unchanged.
    ccd_t ccd;
    add_active(ccd, 3, 260, 7680, 0, 2560, 1440);
    add_active(ccd, 9, 256, 0, 0, 7680, 2160);
    const std::vector<placement_t> back { { os_display_id_t { "{712f4751-2e65-5e95-9578-a4e3b69ce687}" }, position_t { 0, 0 } } };
    const auto refusal = plan_arrange(ccd.paths, ccd.modes, run_5_identify, back, {});
    ASSERT_TRUE(refusal.has_value()) << "the plan was sent: " << ccd_problem(ccd.paths, ccd.modes).value_or("arrays valid");
    EXPECT_NE(refusal->find("cannot identify"), std::string::npos) << *refusal;
    EXPECT_NE(refusal->find("target 256"), std::string::npos) << *refusal;
  }

  TEST(Win32NativeEdge, EveryArrangePlanThatMaySendPassesTheStrictCcdRules) {
    ccd_t ccd;
    add_active(ccd, 3, 260, 0, 0, 2560, 1440);
    add_active(ccd, 3, 261, 2560, 0, 1920, 1080);
    const auto identify = [](const DISPLAYCONFIG_PATH_INFO &path) { return "id-" + std::to_string(path.targetInfo.id); };
    auto into = ccd;
    const std::vector<placement_t> overlapping { { os_display_id_t { "id-261" }, position_t { 1000, 0 } } };
    const auto refusal = plan_arrange(into.paths, into.modes, identify, overlapping, {});
    ASSERT_TRUE(refusal.has_value()) << "an overlapping layout was planned for sending";
    EXPECT_NE(refusal->find("overlap"), std::string::npos) << *refusal;
    auto side = ccd;
    const std::vector<placement_t> beside { { os_display_id_t { "id-261" }, position_t { -1920, 0 } } };
    EXPECT_EQ(plan_arrange(side.paths, side.modes, identify, beside, {}), std::nullopt);
    EXPECT_EQ(ccd_problem(side.paths, side.modes), std::nullopt);
  }
}  // namespace
