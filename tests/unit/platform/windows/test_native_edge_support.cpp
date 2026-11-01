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
}  // namespace
