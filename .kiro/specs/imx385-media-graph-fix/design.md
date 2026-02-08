# Design Document: IMX385 Media Graph Integration Fix

## Overview

This design addresses the media graph integration issue for the Sony IMX385LQR-C sensor on the RV1106 LubanCat-RV06 board. The sensor driver currently loads and communicates via I2C successfully, but fails to appear in the media controller graph due to missing or incorrect device tree endpoint configuration and potential driver initialization issues.

The fix involves:
1. Verifying and correcting device tree endpoint configuration
2. Ensuring proper V4L2 async subdev registration in the driver
3. Validating MIPI CSI-2 register configuration
4. Testing the complete media pipeline from sensor to ISP

## Architecture

### Media Pipeline Topology

The RV1106 camera pipeline follows this topology:

```
IMX385 Sensor (i2c@4-001a)
    ↓ (MIPI CSI-2, 2-lane, RAW10)
csi2_dphy0 (D-PHY receiver)
    ↓
mipi0_csi2 (CSI-2 host controller)
    ↓
rkcif_mipi_lvds (Camera Interface)
    ↓
rkcif_mipi_lvds_sditf (Stream Interface)
    ↓
rkisp_vir0 (ISP Virtual Device)
    ↓
/dev/video11 (ISP output)
```

### Component Responsibilities

- **IMX385 Driver**: V4L2 subdevice that controls the sensor, registers with async framework
- **Device Tree**: Describes hardware connections and endpoint relationships
- **csi2_dphy0**: Physical layer receiver, multiplexes multiple sensor inputs
- **Media Controller**: Kernel framework that manages the device graph and links
- **V4L2 Async**: Mechanism for registering subdevices that may probe in any order

## Components and Interfaces

### 1. Device Tree Endpoint Configuration

**File**: `sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-lubancat-single-cam.dtsi`

**Current Configuration**:
```dts
imx385: imx385@1a {
    compatible = "sony,imx385";
    status = "okay";
    reg = <0x1a>;
    clocks = <&ext_cam_37m_clk>;
    clock-names = "xvclk";
    
    pwdn-gpios = <&gpio5 0 GPIO_ACTIVE_HIGH>;
    reset-gpios = <&gpio5 1 GPIO_ACTIVE_HIGH>;

    dovdd-supply= <&cam_dovdd>;
    avdd-supply = <&cam_avdd>;
    dvdd-supply = <&cam_dvdd>;

    rockchip,camera-module-index = <0>;
    rockchip,camera-module-facing = "back";
    rockchip,camera-module-name = "IMX385LQR";
    rockchip,camera-module-lens-name = "default";

    port {
        imx385_out: endpoint {
            remote-endpoint = <&csi_dphy_input4>;
            data-lanes = <1 2>;
        };
    };
};
```

**Analysis**: The endpoint configuration appears correct. The sensor has:
- A `port` node with an `endpoint` child
- `remote-endpoint` pointing to `csi_dphy_input4`
- `data-lanes` set to `<1 2>` for 2-lane MIPI

**Verification Needed**: Check that `csi_dphy_input4` in `csi2_dphy0` correctly references back to `imx385_out`.

### 2. CSI2 DPHY Configuration

**Current Configuration**:
```dts
&csi2_dphy0 {
    status = "okay";

    ports {
        #address-cells = <1>;
        #size-cells = <0>;

        port@0 {
            reg = <0>;
            #address-cells = <1>;
            #size-cells = <0>;

            csi_dphy_input0: endpoint@0 {
                reg = <0>;
                remote-endpoint = <&ov8858_out>;
                data-lanes = <1 2>;
            };

            csi_dphy_input1: endpoint@1 {
                reg = <1>;
                remote-endpoint = <&sc530ai_out>;
                data-lanes = <1 2>;
            };

            csi_dphy_input2: endpoint@2 {
                reg = <2>;
                remote-endpoint = <&imx415_out>;
                data-lanes = <1 2>;
            };

            csi_dphy_input3: endpoint@3 {
                reg = <3>;
                remote-endpoint = <&gc4653_out>;
                data-lanes = <1 2>;
            };

            csi_dphy_input4: endpoint@4 {
                reg = <4>;
                remote-endpoint = <&imx385_out>;
                data-lanes = <1 2>;
            };
        };

        port@1 {
            reg = <1>;
            #address-cells = <1>;
            #size-cells = <0>;

            csi_dphy_output: endpoint@0 {
                reg = <0>;
                remote-endpoint = <&mipi_csi2_input>;
            };
        };
    };
};
```

**Analysis**: The DPHY configuration looks correct:
- `csi_dphy_input4` has `remote-endpoint = <&imx385_out>` (bidirectional link)
- `data-lanes = <1 2>` matches the sensor configuration
- Output endpoint connects to `mipi_csi2_input`

### 3. IMX385 Driver Media Entity Setup

**File**: `sysdrv/source/kernel/drivers/media/i2c/imx385.c`

**Current Implementation** (from probe function):
```c
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
    dev_err(dev, "set the video v4l2 subdev api\n");
    sd->internal_ops = &imx385_internal_ops;
    sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
                 V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
    dev_err(dev, "set the media controller\n");
    imx385->pad.flags = MEDIA_PAD_FL_SOURCE;
    sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
    ret = media_entity_pads_init(&sd->entity, 1, &imx385->pad);
    if (ret < 0)
        goto err_power_off;
#endif

    // ... snprintf for device name ...

    ret = v4l2_async_register_subdev_sensor_common(sd);
    if (ret) {
        dev_err(dev, "v4l2 async register subdev failed\n");
        goto err_clean_entity;
    }
```

**Analysis**: The driver implementation follows the correct pattern:
1. Sets subdev flags for devnode and events
2. Initializes media pad as source
3. Sets entity function to CAM_SENSOR
4. Calls `v4l2_async_register_subdev_sensor_common()`

**Potential Issue**: The driver uses `dev_err()` for informational messages, which might indicate debugging code left in place.

### 4. Endpoint Parsing

**Current Implementation**:
```c
endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
if (!endpoint) {
    dev_err(dev, "Failed to get endpoint\n");
    return -EINVAL;
}

ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint),
    &imx385->bus_cfg);
if (ret)
    dev_warn(dev, "could not get bus config!\n");
```

**Analysis**: The driver correctly:
1. Gets the endpoint from device tree
2. Parses it using `v4l2_fwnode_endpoint_parse()`
3. Stores bus configuration in `imx385->bus_cfg`

**Potential Issue**: The endpoint node is not released with `of_node_put()` after parsing.

### 5. MIPI CSI-2 Register Configuration

**Current Register Settings** (from `imx385_linear_1920x1080_mipi_regs`):
```c
{0x3044, 0x00},  // ODBIT = 10-bit output
{0x3346, 0x03},  // PHYSICAL_LANE_NUM = 4-Lane
{0x337F, 0x03},  // CSI_LANE_MODE = 4-Lane
{0x337D, 0x0A},  // CSI_DT_FMT = RAW10 low byte
{0x337E, 0x0A},  // CSI_DT_FMT = RAW10 high byte
```

**Issue Identified**: The MIPI register configuration is set for 4-lane mode, but the device tree specifies 2-lane mode (`data-lanes = <1 2>`).

**Required Changes**:
- For 2-lane MIPI: `0x3346 = 0x01`, `0x337F = 0x01`
- Or update device tree to use 4-lane: `data-lanes = <1 2 3 4>`

## Data Models

### V4L2 Subdevice Structure

```c
struct imx385 {
    struct i2c_client       *client;
    struct v4l2_subdev      subdev;          // V4L2 subdevice
    struct media_pad        pad;             // Single source pad
    struct v4l2_fwnode_endpoint bus_cfg;     // Bus configuration from DT
    const struct imx385_mode *cur_mode;      // Current mode
    // ... other fields ...
};
```

### Device Tree Endpoint Structure

```
sensor {
    port {
        endpoint {
            remote-endpoint = <&dphy_input>;
            data-lanes = <1 2>;
            // Optional: clock-lanes, link-frequencies
        };
    };
};
```

### Media Graph Entities

```
Entity 0: IMX385 (1 pad, 1 link)
  - Pad 0: Source
    - Link 0: IMX385:0 -> csi2_dphy0:4 [ENABLED, IMMUTABLE]

Entity 1: csi2_dphy0 (6 pads, 5 links)
  - Pad 0-4: Sink (for sensors)
  - Pad 5: Source
    - Link: csi2_dphy0:5 -> mipi0_csi2:0 [ENABLED, IMMUTABLE]

Entity 2: mipi0_csi2 (2 pads, 2 links)
  - Pad 0: Sink
  - Pad 1: Source
    - Link: mipi0_csi2:1 -> rkcif_mipi_lvds:0 [ENABLED, IMMUTABLE]

// ... continues through ISP ...
```

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system—essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*


### Property Reflection

After analyzing all acceptance criteria, I identified the following redundancies:

**Redundant Properties:**
- 2.4 is redundant with 2.2 (both check bidirectional endpoint references)
- 4.2 is redundant with 3.4 (both check source pad configuration)
- 5.2 is redundant with 1.4 (both check for the same error message)
- 10.5 is redundant with 10.2 and 10.3 (covered by checking specific register values)

**Properties to Keep:**
- 2.5: Unique endpoint register numbers (this is a general property across all sensors)
- All other criteria are specific examples or edge cases that provide unique validation value

### Correctness Properties

Property 1: Unique Endpoint Register Numbers
*For any* device tree configuration with multiple camera sensors, all endpoint register numbers within the same port SHALL be unique to prevent conflicts.
**Validates: Requirements 2.5**

Property 2: Media Graph Entity Presence
*For any* correctly configured camera sensor with valid device tree endpoints and successful driver probe, the sensor entity SHALL appear in the media controller graph output.
**Validates: Requirements 1.1, 1.2, 1.3**

Property 3: Bidirectional Endpoint References
*For any* sensor-to-DPHY endpoint connection in the device tree, both the sensor endpoint and the DPHY endpoint SHALL reference each other via remote-endpoint properties.
**Validates: Requirements 2.1, 2.2, 2.4**

Property 4: MIPI Lane Configuration Consistency
*For any* MIPI CSI-2 sensor configuration, the data-lanes property in the device tree SHALL match the lane configuration registers (0x3346, 0x337F) programmed in the sensor.
**Validates: Requirements 2.3, 10.2, 10.3, 10.5**

Property 5: Media Pipeline Connectivity
*For any* functional camera system, the media graph SHALL show a complete connected path from the sensor entity through all intermediate entities (DPHY, CSI-2, CIF, SDITF) to the ISP entity.
**Validates: Requirements 1.5**

Property 6: V4L2 Subdev Registration Success
*For any* camera sensor driver that successfully probes and detects the sensor hardware, the v4l2_async_register_subdev_sensor_common call SHALL return success and create a /dev/v4l-subdevX device node.
**Validates: Requirements 3.1, 3.6**

Property 7: Media Entity Initialization
*For any* camera sensor driver with CONFIG_MEDIA_CONTROLLER enabled, the driver SHALL initialize exactly one media pad with MEDIA_PAD_FL_SOURCE flag and set entity function to MEDIA_ENT_F_CAM_SENSOR.
**Validates: Requirements 3.4, 3.5, 4.1, 4.2**

Property 8: Endpoint Parsing Success
*For any* camera sensor with a valid device tree endpoint node, the driver SHALL successfully parse the endpoint using v4l2_fwnode_endpoint_parse and store the bus configuration including bus type and lane count.
**Validates: Requirements 4.3, 4.4, 4.5**

Property 9: ISP Data Capture
*For any* correctly configured camera pipeline with established media links, capturing data from /dev/video11 SHALL produce non-zero data output without "get remote terminal sensor failed" errors.
**Validates: Requirements 1.4, 5.1, 5.2**

Property 10: Backward Compatibility
*For any* existing working camera sensor configuration (ov8858, sc530ai, imx415, gc4653), applying the IMX385 fix SHALL NOT cause the existing sensor to fail probe, media graph registration, or data capture.
**Validates: Requirements 7.1, 7.2, 7.3, 7.4**

## Error Handling

### Device Tree Errors

**Missing Endpoint Node**:
- **Detection**: `of_graph_get_next_endpoint()` returns NULL
- **Handling**: Driver probe fails with -EINVAL, logs "Failed to get endpoint"
- **Recovery**: Fix device tree to include port/endpoint nodes

**Invalid Remote Endpoint Reference**:
- **Detection**: Endpoint parsing succeeds but remote endpoint doesn't exist
- **Handling**: Media graph links fail to establish, sensor not visible in media-ctl
- **Recovery**: Verify bidirectional endpoint references in device tree

**Lane Count Mismatch**:
- **Detection**: Device tree specifies different lane count than sensor registers
- **Handling**: MIPI CSI-2 communication fails, no data received
- **Recovery**: Align data-lanes property with sensor register configuration

### Driver Initialization Errors

**I2C Communication Failure**:
- **Detection**: `imx385_check_sensor_id()` fails to read chip ID
- **Handling**: Driver probe fails, logs I2C error code and address
- **Recovery**: Check I2C bus, address, power supply, and clock

**Media Entity Registration Failure**:
- **Detection**: `media_entity_pads_init()` returns error
- **Handling**: Driver probe fails, cleans up allocated resources
- **Recovery**: Check kernel configuration for CONFIG_MEDIA_CONTROLLER

**Async Subdev Registration Failure**:
- **Detection**: `v4l2_async_register_subdev_sensor_common()` returns error
- **Handling**: Driver probe fails, cleans up media entity
- **Recovery**: Check for conflicting drivers or missing platform components

### Runtime Errors

**Media Link Establishment Failure**:
- **Detection**: Sensor appears in media graph but no links to DPHY
- **Handling**: ISP cannot access sensor, "get remote terminal sensor failed" error
- **Recovery**: Verify endpoint configuration and driver registration order

**MIPI CSI-2 Data Reception Failure**:
- **Detection**: ISP receives 0 bytes when capturing from /dev/video11
- **Handling**: Video capture fails or produces black frames
- **Recovery**: Check MIPI lane configuration, clock, and register settings

**UVC Streaming Failure**:
- **Detection**: UVC application fails to initialize or stream
- **Handling**: No video appears on host PC
- **Recovery**: Verify complete media pipeline, ISP configuration, and UVC setup

## Testing Strategy

### Dual Testing Approach

This fix requires both **unit tests** and **integration tests** to ensure correctness:

**Unit Tests**: Verify specific components in isolation
- Device tree parsing and validation
- Driver probe sequence
- Register configuration
- Media entity initialization

**Integration Tests**: Verify end-to-end functionality
- Media graph topology
- Data capture from ISP
- UVC video streaming
- Backward compatibility with existing sensors

### Test Categories

#### 1. Device Tree Validation Tests

**Purpose**: Verify device tree configuration is correct before driver loads

**Tests**:
- Parse device tree and verify IMX385 node exists with required properties
- Verify endpoint node has remote-endpoint and data-lanes properties
- Verify csi2_dphy0 has corresponding endpoint referencing IMX385
- Verify all endpoint register numbers are unique
- Verify clock, GPIO, and regulator references are valid

**Tools**: `dtc` (device tree compiler), custom DT parsing scripts

#### 2. Driver Probe Tests

**Purpose**: Verify driver initialization sequence

**Tests**:
- Mock I2C to verify chip ID detection logic
- Verify endpoint parsing extracts correct bus configuration
- Verify media entity initialization with correct pad and function
- Verify V4L2 async subdev registration succeeds
- Verify /dev/v4l-subdevX device node is created

**Tools**: Kernel unit test framework, mock I2C driver

#### 3. Media Graph Integration Tests

**Purpose**: Verify sensor appears in media controller graph

**Tests**:
- Execute `media-ctl -p -d /dev/media0` and verify IMX385 entity present
- Execute `media-ctl -p -d /dev/media1` and verify ISP pipeline with IMX385
- Verify sensor name format matches "m00_b_imx385 4-001a"
- Verify complete pipeline connectivity from sensor to ISP
- Verify no "get remote terminal sensor failed" errors in dmesg

**Tools**: `media-ctl`, dmesg parsing, shell scripts

#### 4. MIPI Configuration Tests

**Purpose**: Verify MIPI CSI-2 registers are configured correctly

**Tests**:
- Read register 0x3044 and verify value is 0x00 (10-bit)
- Read register 0x3346 and verify lane count matches device tree
- Read register 0x337F and verify lane mode matches device tree
- Read registers 0x337D/0x337E and verify RAW10 format (0x0A)
- Test both 2-lane and 4-lane configurations

**Tools**: I2C register read utilities, sensor debug interface

#### 5. Data Capture Tests

**Purpose**: Verify ISP can capture data from sensor

**Tests**:
- Configure media pipeline using media-ctl
- Execute `timeout 5 cat /dev/video11 > /tmp/test.raw`
- Verify output file size > 100KB
- Verify no errors in dmesg during capture
- Verify data format is RAW10 Bayer (GBRG pattern)

**Tools**: `media-ctl`, `cat`, file size check, Bayer pattern validator

#### 6. UVC Streaming Tests

**Purpose**: Verify end-to-end video streaming

**Tests**:
- Start UVC_TINY application
- Verify application initializes without errors
- Connect host PC and open OBS/VLC
- Verify video feed appears on host
- Verify stable streaming for 60 seconds
- Measure frame rate and check for drops

**Tools**: UVC_TINY application, OBS, VLC, frame rate monitor

#### 7. Backward Compatibility Tests

**Purpose**: Verify existing sensors still work after IMX385 fix

**Tests**:
- For each sensor (ov8858, sc530ai, imx415, gc4653):
  - Verify driver probes successfully
  - Verify sensor appears in media graph
  - Verify data capture works
  - Verify no regression in functionality

**Tools**: Same as media graph and data capture tests, applied to each sensor

#### 8. Build System Tests

**Purpose**: Verify build configuration includes IMX385 driver

**Tests**:
- Build with BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
- Verify CONFIG_VIDEO_IMX385=y in .config
- Verify imx385.ko is built
- Verify device tree includes IMX385 configuration
- Verify final image boots and loads driver

**Tools**: Build scripts, kernel config checker, module list

### Test Execution Order

1. **Device Tree Validation** (before build)
2. **Build System Tests** (during build)
3. **Driver Probe Tests** (after boot, before sensor access)
4. **Media Graph Integration Tests** (after driver probe)
5. **MIPI Configuration Tests** (after driver probe)
6. **Data Capture Tests** (after media graph verified)
7. **UVC Streaming Tests** (after data capture verified)
8. **Backward Compatibility Tests** (final verification)

### Success Criteria

The fix is considered successful when:
1. All unit tests pass
2. IMX385 appears in media-ctl output
3. ISP captures > 100KB of data
4. UVC streams video to host PC
5. All existing sensors continue to work
6. No errors in dmesg related to media graph or sensor

### Test Automation

**Automated Tests**:
- Device tree validation (shell script)
- Build system tests (CI/CD pipeline)
- Media graph presence (shell script with media-ctl)
- Data capture size check (shell script)
- Backward compatibility (shell script loop)

**Manual Tests**:
- UVC video quality verification (visual inspection in OBS/VLC)
- Frame rate stability (manual monitoring)
- Long-term streaming stability (overnight test)

### Test Environment

**Hardware**:
- RV1106 LubanCat-RV06 development board
- Sony IMX385LQR-C sensor module
- USB connection to host PC
- Power supply (5V)

**Software**:
- RV1106 Linux SDK (kernel 5.10.160)
- media-ctl utility
- UVC_TINY application
- OBS or VLC on host PC

**Build Configuration**:
- BoardConfig-SPI_NAND-NONE-RV1106_LubanCat-RV06.mk
- CONFIG_VIDEO_IMX385=y
- CONFIG_MEDIA_CONTROLLER=y
- CONFIG_VIDEO_V4L2_SUBDEV_API=y
