# Implementation Plan: IMX385 Media Graph Integration Fix

## Overview

This implementation plan addresses the media graph integration issue for the Sony IMX385LQR-C sensor on the RV1106 LubanCat-RV06 board. The fix involves verifying device tree configuration, ensuring proper driver initialization, validating MIPI CSI-2 register settings, and testing the complete camera pipeline.

## Tasks

- [ ] 1. Verify and document current system state
  - Capture current dmesg output showing driver probe
  - Capture current media-ctl output for /dev/media0 and /dev/media1
  - Document current I2C communication status
  - Verify sensor chip ID detection is working
  - _Requirements: 1.1, 1.2, 9.1_

- [ ] 2. Analyze device tree configuration
  - [ ] 2.1 Verify IMX385 endpoint configuration
    - Check that imx385 node has port/endpoint with remote-endpoint property
    - Verify data-lanes property is set to <1 2>
    - Verify remote-endpoint points to csi_dphy_input4
    - _Requirements: 2.1, 2.3_
  
  - [ ] 2.2 Verify csi2_dphy0 endpoint configuration
    - Check that csi_dphy_input4 exists with correct reg number
    - Verify remote-endpoint points back to imx385_out
    - Verify data-lanes matches sensor configuration
    - _Requirements: 2.2, 2.4_
  
  - [ ] 2.3 Verify endpoint register number uniqueness
    - List all endpoint@N nodes in csi2_dphy0 port@0
    - Confirm all N values are unique (0-4 for current sensors)
    - _Requirements: 2.5_

- [ ] 3. Identify MIPI lane configuration mismatch
  - [ ] 3.1 Check device tree lane configuration
    - Verify data-lanes property in both sensor and DPHY endpoints
    - Document whether 2-lane or 4-lane is configured
    - _Requirements: 2.3_
  
  - [ ] 3.2 Check sensor register configuration
    - Review imx385_linear_1920x1080_mipi_regs array
    - Check register 0x3346 (PHYSICAL_LANE_NUM) value
    - Check register 0x337F (CSI_LANE_MODE) value
    - Identify mismatch between DT (2-lane) and registers (4-lane)
    - _Requirements: 10.2, 10.3_
  
  - [ ] 3.3 Determine correct lane configuration
    - Check hardware capability (sensor and board support)
    - Decide whether to use 2-lane or 4-lane MIPI
    - Document decision and rationale
    - _Requirements: 10.2, 10.3, 10.5_

- [ ] 4. Fix MIPI CSI-2 register configuration for 2-lane mode
  - [ ] 4.1 Update linear mode register array
    - Modify imx385_linear_1920x1080_mipi_regs
    - Change 0x3346 from 0x03 to 0x01 (2-lane physical)
    - Change 0x337F from 0x03 to 0x01 (2-lane mode)
    - Verify 0x3044 is 0x00 (10-bit output)
    - Verify 0x337D and 0x337E are 0x0A (RAW10)
    - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5_
  
  - [ ] 4.2 Update HDR mode register array
    - Modify imx385_hdr2_1920x1080_mipi_regs
    - Change 0x3346 from 0x03 to 0x01 (2-lane physical)
    - Change 0x337F from 0x03 to 0x01 (2-lane mode)
    - Verify other MIPI registers match linear mode
    - _Requirements: 10.2, 10.3, 10.5_

- [ ] 5. Review driver media entity initialization
  - [ ] 5.1 Verify V4L2 subdev flags
    - Check that V4L2_SUBDEV_FL_HAS_DEVNODE is set
    - Check that V4L2_SUBDEV_FL_HAS_EVENTS is set
    - Verify flags are set before async registration
    - _Requirements: 3.2, 3.3_
  
  - [ ] 5.2 Verify media pad initialization
    - Check that pad.flags = MEDIA_PAD_FL_SOURCE
    - Check that entity.function = MEDIA_ENT_F_CAM_SENSOR
    - Verify media_entity_pads_init is called with 1 pad
    - _Requirements: 3.4, 3.5, 4.1_
  
  - [ ] 5.3 Verify endpoint parsing
    - Check that of_graph_get_next_endpoint is called
    - Check that v4l2_fwnode_endpoint_parse is called
    - Verify bus_cfg is stored correctly
    - Add of_node_put to release endpoint node
    - _Requirements: 4.3, 4.4, 4.5_
  
  - [ ] 5.4 Verify async subdev registration
    - Check that v4l2_async_register_subdev_sensor_common is called
    - Verify it's called after media entity initialization
    - Check error handling and cleanup on failure
    - _Requirements: 3.1, 3.6_

- [ ] 6. Checkpoint - Build and test initial fix
  - Build kernel with updated IMX385 driver
  - Flash image to board and boot
  - Check dmesg for driver probe messages
  - Run media-ctl -p -d /dev/media0 and /dev/media1
  - Verify IMX385 appears in media graph
  - Ensure all tests pass, ask the user if questions arise.
  - _Requirements: 1.1, 1.2, 1.3, 8.1, 8.2, 8.3, 8.4, 8.5_

- [ ] 7. Test media graph connectivity
  - [ ] 7.1 Verify sensor entity presence
    - Execute media-ctl -p -d /dev/media0
    - Confirm IMX385 entity appears with name "m00_b_imx385 4-001a"
    - Verify entity has 1 source pad
    - _Requirements: 1.1, 1.3_
  
  - [ ] 7.2 Verify ISP pipeline
    - Execute media-ctl -p -d /dev/media1
    - Confirm complete pipeline: IMX385 → csi2_dphy0 → mipi0_csi2 → rkcif_mipi_lvds → rkcif_mipi_lvds_sditf → rkisp_vir0
    - Verify all links are ENABLED and IMMUTABLE
    - _Requirements: 1.2, 1.5_
  
  - [ ] 7.3 Check for error messages
    - Review dmesg output
    - Confirm no "get remote terminal sensor failed" errors
    - Confirm no media graph registration errors
    - _Requirements: 1.4, 9.2_

- [ ] 8. Test ISP data capture
  - [ ] 8.1 Configure media pipeline
    - Use media-ctl to set format on sensor pad
    - Use media-ctl to set format on ISP pads
    - Enable media links if needed
    - _Requirements: 5.3_
  
  - [ ] 8.2 Capture raw data from ISP
    - Execute: timeout 5 cat /dev/video11 > /tmp/test.raw
    - Check file size: ls -lh /tmp/test.raw
    - Verify file size > 100KB
    - _Requirements: 5.1_
  
  - [ ] 8.3 Validate data format
    - Check that data is RAW10 Bayer format
    - Verify GBRG Bayer pattern (first row Gb-B, second row R-Gr)
    - Confirm data is not all zeros
    - _Requirements: 5.5_
  
  - [ ] 8.4 Verify streaming behavior
    - Check that buffers are allocated
    - Verify data transfer begins
    - Monitor for errors during streaming
    - _Requirements: 5.4_

- [ ] 9. Test UVC video streaming
  - [ ] 9.1 Start UVC application
    - Launch UVC_TINY application
    - Verify application initializes without errors
    - Check that ISP video device opens successfully
    - _Requirements: 6.1, 6.5_
  
  - [ ] 9.2 Verify USB video transmission
    - Connect board to host PC via USB
    - Check that UVC device appears on host
    - Verify data is transmitted over USB
    - _Requirements: 6.2_
  
  - [ ] 9.3 Test video display on host
    - Open OBS or VLC on host PC
    - Select UVC camera device
    - Verify video feed appears
    - Check video quality and frame rate
    - _Requirements: 6.3_
  
  - [ ] 9.4 Test streaming stability
    - Stream video for 60 seconds
    - Monitor for frame drops or errors
    - Verify stable continuous streaming
    - _Requirements: 6.4_

- [ ] 10. Checkpoint - Verify IMX385 fully functional
  - Confirm sensor appears in media graph
  - Confirm ISP captures > 100KB data
  - Confirm UVC streams video to host
  - Confirm no errors in dmesg
  - Ensure all tests pass, ask the user if questions arise.
  - _Requirements: 1.1, 1.2, 1.4, 5.1, 6.2_

- [ ] 11. Test backward compatibility
  - [ ] 11.1 Test ov8858 sensor
    - Boot with ov8858 enabled in device tree
    - Verify driver probes successfully
    - Verify sensor appears in media graph
    - Verify data capture works
    - _Requirements: 7.1_
  
  - [ ] 11.2 Test sc530ai sensor
    - Boot with sc530ai enabled in device tree
    - Verify driver probes successfully
    - Verify sensor appears in media graph
    - Verify data capture works
    - _Requirements: 7.2_
  
  - [ ] 11.3 Test imx415 sensor
    - Boot with imx415 enabled in device tree
    - Verify driver probes successfully
    - Verify sensor appears in media graph
    - Verify data capture works
    - _Requirements: 7.3_
  
  - [ ] 11.4 Test gc4653 sensor
    - Boot with gc4653 enabled in device tree
    - Verify driver probes successfully
    - Verify sensor appears in media graph
    - Verify data capture works
    - _Requirements: 7.4_

- [ ] 12. Improve diagnostic logging
  - [ ] 12.1 Update probe success messages
    - Change dev_err to dev_info for successful operations
    - Ensure "IMX385 sensor detected successfully" message includes chip ID
    - Add power-on sequence completion message
    - _Requirements: 9.1_
  
  - [ ] 12.2 Enhance error messages
    - Add descriptive error for media graph registration failure
    - Add specific device tree issue logging for endpoint parsing
    - Add I2C address and error code to I2C failure messages
    - _Requirements: 9.2, 9.3, 9.5_

- [ ] 13. Final validation and documentation
  - [ ] 13.1 Run complete test suite
    - Execute all media graph tests
    - Execute all data capture tests
    - Execute all UVC streaming tests
    - Execute all backward compatibility tests
    - _Requirements: All_
  
  - [ ] 13.2 Document test results
    - Record media-ctl output showing IMX385 in graph
    - Record successful data capture file sizes
    - Record UVC streaming success
    - Record backward compatibility test results
    - _Requirements: All_
  
  - [ ] 13.3 Create verification checklist
    - List all requirements and their validation status
    - Document any known limitations or issues
    - Provide troubleshooting guide for common problems
    - _Requirements: All_

- [ ] 14. Final checkpoint - Complete validation
  - All requirements validated
  - All tests passing
  - Documentation complete
  - Ready for production use
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Backward compatibility tests ensure no regressions
- The fix focuses on device tree and register configuration, not major driver changes
