# 图像数据说明 {#spec_image_data}

| 名称 | 字段 | 单位 | 字节数 | 说明 |
| :----- | :----- | :----- | :-------- | :----- |
| 帧 ID | frame_id | - | 2 | uint16_t; [0,65535] |
| 时间戳 | timestamp | 10 us | 4 | uint32_t |
| 曝光时间 | exposure_time | 10 us | 2 | uint16_t |

> 图像数据传输方式待调研： frame header 或占用像素末尾。

## 图像数据包

> 大端序。如果传输方式是占用像素末尾，则数据包倒序排在尾部。

| Name | Header | Size | FrameID | Timestamp | ExposureTime | Checksum |
| :--- | :----- | :--- | :------ | :-------- | :----------- | :------- |
| 字节数 | 1 | 1 | 2 | 4 | 2 | 1 |
| 类型 | uint8_t | uint8_t | uint16_t | uint32_t | uint16_t | uint8_t |
| 描述 | 0x3B | 0x0B （数据包大小） | 帧 ID | 时间戳 | 曝光时间 | 校验码（Header 以外所有包字节异或） |

* 数据包校验不过，会丢弃该帧。
* 时间的单位精度为： 0.01 ms / 10 us 。
  * 4 字节能表示的最大时间约是 11.9 小时，溢出后将重累计。
* 时间累计是从上电时从开始，而不是从打开时开始。