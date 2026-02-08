# Requirements Document

## Introduction

This specification addresses the media graph integration issue for the Sony IMX385LQR-C image sensor on the RV1106 LubanCat-RV06 development board. The sensor driver loads successfully and I2C communication works, but the sensor does not appear in the media controller graph, preventing the ISP from capturing data. The root cause is that media graph links are not being established between the sensor and the camera pipeline components.

## Glossary

- **IMX385**: Sony IMX385LQR-C CMOS image sensor with MIPI CSI-2 interface
- **Media_Graph**: Linux kernel media controller framework topology showing device connections
- **DPHY**: D-PHY physical layer for MIPI CSI-2 interface
- **MIPI_CSI2**: Mobile Industry Processor Interface Camera Serial Interface 2
- **CIF**: Camera Interface module (rkcif) that receives data from MIPI CSI-2
- **ISP**: Image Signal Processor (rkisp) that processes raw sensor data
- **Device_Tree**: Hardware description format used to configure kernel drivers
- **Async_Subdev**: V4L2 asynchronous subdevice registration mechanism
- **Endpoint**: Device tree node describing a connection point in the media graph
- **Remote_Endpoint**: Reference to the other side of a media graph connection
- **Media_Entity**: A node in the media controller graph (sensor, ISP, etc.)
- **Media_Pad**: Connection point on a media entity (source or sink)
- **Media_Link**: Connection between two media pads in the graph

## Requirements

### Requirement 1: Media Graph Visibility

**User Story:** As a system developer, I want the IMX385 sensor to appear in the media controller graph, so that I can verify the camera pipeline topology.

#### Acceptance Criteria

1. WHEN executing `media-ctl -p -d /dev/media0`, THE System SHALL display the IMX385 sensor entity in the output
2. WHEN executing `media-ctl -p -d /dev/media1`, THE System SHALL display the complete ISP pipeline with IMX385 as the source
3. THE System SHALL show the sensor name in the format "m00_b_imx385 4-001a" in the media graph
4. WHEN the sensor driver loads, THE System SHALL NOT produce "get remote terminal sensor failed" errors in dmesg
5. THE Media_Graph SHALL show connections from IMX385 through csi2_dphy0, mipi0_csi2, rkcif_mipi_lvds, rkcif_mipi_lvds_sditf to rkisp_vir0

### Requirement 2: Device Tree Configuration

**User Story:** As a system developer, I want correct device tree endpoint configuration, so that the kernel can establish media graph links.

#### Acceptance Criteria

1. THE Device_Tree SHALL define an endpoint node for the IMX385 sensor with a remote-endpoint property
2. THE Device_Tree SHALL define a corresponding endpoint in csi2_dphy0 that references the IMX385 endpoint
3. WHEN the IMX385 endpoint is defined, THE System SHALL specify data-lanes property as <1 2> for 2-lane MIPI
4. THE Device_Tree SHALL maintain bidirectional endpoint references between IMX385 and csi2_dphy0
5. WHEN multiple sensors are configured, THE System SHALL assign unique endpoint register numbers to avoid conflicts

### Requirement 3: V4L2 Async Subdev Registration

**User Story:** As a kernel developer, I want proper V4L2 async subdev registration, so that the media controller can discover and link the sensor.

#### Acceptance Criteria

1. WHEN the IMX385 driver probes, THE System SHALL call v4l2_async_register_subdev_sensor_common successfully
2. WHEN registering the subdev, THE System SHALL set the V4L2_SUBDEV_FL_HAS_DEVNODE flag
3. WHEN registering the subdev, THE System SHALL set the V4L2_SUBDEV_FL_HAS_EVENTS flag
4. THE System SHALL initialize the media pad with MEDIA_PAD_FL_SOURCE flag
5. THE System SHALL set the entity function to MEDIA_ENT_F_CAM_SENSOR
6. WHEN registration completes, THE System SHALL create a /dev/v4l-subdevX device node for the sensor

### Requirement 4: Media Entity Configuration

**User Story:** As a kernel developer, I want correct media entity setup, so that the sensor integrates into the media controller framework.

#### Acceptance Criteria

1. WHEN CONFIG_MEDIA_CONTROLLER is enabled, THE System SHALL call media_entity_pads_init with 1 pad
2. THE System SHALL configure the sensor's single pad as a source pad
3. WHEN the driver initializes, THE System SHALL parse the device tree endpoint using v4l2_fwnode_endpoint_parse
4. THE System SHALL store the bus configuration including bus type and lane count
5. WHEN the sensor is MIPI CSI-2, THE System SHALL configure bus_type as V4L2_MBUS_CSI2_DPHY

### Requirement 5: ISP Data Capture

**User Story:** As a system developer, I want the ISP to capture non-zero data from the sensor, so that I can verify the camera pipeline is functional.

#### Acceptance Criteria

1. WHEN executing `timeout 5 cat /dev/video11 > /tmp/test.raw`, THE System SHALL produce a file larger than 100KB
2. WHEN the ISP captures data, THE System SHALL NOT produce "rkcif_update_sensor_info: stream[0] get remote terminal sensor failed" errors
3. THE System SHALL successfully configure the media pipeline format using media-ctl commands
4. WHEN streaming starts, THE System SHALL allocate buffers and begin data transfer
5. THE System SHALL produce valid RAW10 Bayer data from the sensor

### Requirement 6: UVC Video Streaming

**User Story:** As an end user, I want UVC video streaming to work, so that I can view the camera feed on a host computer.

#### Acceptance Criteria

1. WHEN the UVC application starts, THE System SHALL successfully open the ISP video device
2. WHEN UVC streams video, THE System SHALL transmit data to the host PC via USB
3. WHEN opening OBS or VLC on the host, THE System SHALL display the camera feed
4. THE System SHALL maintain stable video streaming without frame drops or errors
5. WHEN the media graph is correct, THE UVC_TINY application SHALL successfully initialize the camera pipeline

### Requirement 7: Backward Compatibility

**User Story:** As a system developer, I want existing camera configurations to continue working, so that I don't break other sensors.

#### Acceptance Criteria

1. WHEN the IMX385 fix is applied, THE System SHALL NOT break ov8858 sensor functionality
2. WHEN the IMX385 fix is applied, THE System SHALL NOT break sc530ai sensor functionality
3. WHEN the IMX385 fix is applied, THE System SHALL NOT break imx415 sensor functionality
4. WHEN the IMX385 fix is applied, THE System SHALL NOT break gc4653 sensor functionality
5. THE System SHALL support multiple sensor configurations through device tree overlays or conditional compilation

### Requirement 8: Build System Integration

**User Story:** As a system developer, I want the build system to correctly compile the IMX385 driver, so that the sensor is available in the kernel.

#### Acceptance Criteria

1. WHEN building with BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk, THE System SHALL include the IMX385 driver
2. THE System SHALL enable CONFIG_VIDEO_IMX385 in the kernel configuration
3. WHEN the kernel builds, THE System SHALL compile the IMX385 driver without errors
4. THE System SHALL include the device tree with IMX385 configuration in the final image
5. WHEN the system boots, THE System SHALL load the IMX385 driver and detect the sensor

### Requirement 9: Diagnostic and Debugging

**User Story:** As a system developer, I want clear diagnostic messages, so that I can troubleshoot media graph issues.

#### Acceptance Criteria

1. WHEN the sensor driver probes, THE System SHALL log "IMX385 sensor detected successfully" with chip ID
2. WHEN media graph registration fails, THE System SHALL log descriptive error messages
3. WHEN endpoint parsing fails, THE System SHALL log the specific device tree issue
4. THE System SHALL provide media-ctl commands to inspect the media graph topology
5. WHEN I2C communication fails, THE System SHALL log the I2C address and error code

### Requirement 10: MIPI CSI-2 Configuration

**User Story:** As a hardware engineer, I want correct MIPI CSI-2 register configuration, so that the sensor outputs data in the correct format.

#### Acceptance Criteria

1. THE System SHALL configure register 0x3044 to 0x00 for 10-bit output
2. THE System SHALL configure register 0x3346 to 0x03 for 4-lane mode (or appropriate value for 2-lane)
3. THE System SHALL configure register 0x337F to match the lane configuration
4. THE System SHALL configure registers 0x337D and 0x337E to 0x0A for RAW10 format
5. WHEN using 2-lane MIPI, THE System SHALL adjust lane configuration registers accordingly
