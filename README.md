# Doomclock

นาฬิกาปลุก + กล่องขังมือถือ ปลดล็อกเมื่อ AI ยืนยันว่าลุกจากเตียงจริงครบ 30 นาที
Embedded System (CEDT) Final Project 2026

## โครงสร้าง

```
include/doom_protocol.h   รูปแบบ packet ESP-NOW (node และ gateway ใช้ไฟล์เดียวกัน)
include/secrets.example.h ต้นแบบ secrets.h (ห้าม commit secrets.h)
src/node/                 firmware ของ bed sensor node
src/gateway/              firmware ของนาฬิกา
Arduino_Code/main.ino     ร่างแรกของฝั่ง cloud (เก็บไว้อ้างอิง ไม่ได้ build)
```

## เริ่มใช้

1. คัดลอก `include/secrets.example.h` เป็น `include/secrets.h` แล้วใส่ Wi-Fi, NETPIE key, Discord webhook
2. Upload:
   - `pio run -e node -t upload`
   - `pio run -e gateway -t upload`
   (ใน VS Code เลือก env ได้จากแถบล่างของ PlatformIO)
3. เปิด Serial Monitor (115200) ทั้งสองบอร์ด
   - node จะพิมพ์ `[WIN] ...` ทุก 10 วินาที
   - gateway จะพิมพ์ `[LINK] gateway ...` เมื่อเจอกัน
   - พิมพ์ `s` ที่ gateway เพื่อดูสถิติ

node จะสแกนหา channel ของ gateway เอง เพราะ gateway ใช้ channel เดียวกับ hotspot ที่ต่ออยู่ ไม่ต้องตั้ง channel ด้วยมือ

- Wi-Fi ต้องเป็น 2.4 GHz (ถ้าใช้ hotspot ของ iPhone ให้เปิด Maximize Compatibility)
- ห้ามใช้ key NETPIE ชุดเดียวกับบอร์ดอื่นที่เปิดอยู่ ไม่งั้นจะเตะกันหลุด
- ถ้า upload แล้วขึ้น `Waiting for packet header` ให้กดปุ่ม BOOT ค้างตอนที่ขึ้น `Connecting...`

## Gateway: เช็กว่าทำงานปกติ

ตอนบูตควรเห็นใน Serial Monitor:

```
[HW] BH1750 ok, hall baseline 1950
[TIME] DS3231 ok, clock set from RTC     (ครั้งแรกอาจเป็น unknown until NTP)
[CLOUD] NETPIE connected
[TIME] NTP sync -> DS3231 updated
[DISCORD] HTTP 204                       (204 = ส่งสำเร็จ)
```

หลังจากนั้น gateway ส่ง record ขึ้น NETPIE ทุก 10 วินาที gateway ไม่พิมพ์อะไรทุกครั้งที่ได้รับ packet จาก node ให้พิมพ์ `s` แล้วดูว่า `rx` เพิ่มขึ้นไหม

| คำสั่ง Serial | ผล |
|---|---|
| `s` | สถิติทั้งหมด (sensor, ถาด, node, rx/lost/dup, cloud, คิวออฟไลน์, ข้อมูลเตียงล่าสุด) |
| `h` | ค่าดิบ Hall เทียบกับ baseline |
| `c` | บันทึก baseline ของ Hall (ทำตอนแม่เหล็กอยู่ห่าง) |
| `d` | ทดสอบ Discord |

| อาการ | สาเหตุที่น่าจะเป็น |
|---|---|
| `Wi-Fi reconnecting` วนไม่หยุด | SSID/รหัสผิด หรือเป็น Wi-Fi 5 GHz |
| `NETPIE connect failed, state 4` หรือ `5` | client ID / token / secret ผิด |
| `state -2` | ต่อ server ไม่ได้ (เน็ตไม่ออก หรือ TLS ล้มเหลว) |
| connected แล้วหลุดวนไปมา | มีบอร์ดอื่นใช้ key NETPIE ชุดเดียวกัน |
| `BH1750 / DS3231 NOT FOUND` | สาย SDA/SCL สลับหรือหลวม |
| `tempCelsius` เป็น null | DHT11 อ่านไม่ได้ (เช็กขา 27) |
| `time_local` เป็นปี 1970 | ยังไม่เคยตั้งเวลาและยังไม่ได้ sync NTP |
| `[DISCORD] HTTP 401/404` | URL ของ webhook ผิด |

## ต่อสาย

### Bed node (ESP32 DevKit V1)

| อุปกรณ์ | ขา | ESP32 |
|---|---|---|
| MPU6050 | SDA / SCL / VCC / GND | 21 / 22 / 3V3 / GND |
| VL53L0X | SDA / SCL / VIN / GND | 21 / 22 / 3V3 / GND |
| KY-038 (ถ้าใช้) | AO | 34 |
| SD module (ถ้าใช้) | CS / SCK / MISO / MOSI | 5 / 18 / 19 / 23 |

ตอนบูต node จะ scan I2C และพิมพ์ address ที่เจอ ต้องเห็น 0x68 (MPU6050) และ 0x29 (VL53L0X)
- ถ้าไม่เจอ 0x29 แปลว่าตัวที่ยืมมาไม่ใช่ VL53L0X
- ไมค์กับ SD เปิดได้ที่ `ENABLE_MIC` / `ENABLE_SD` ใน `src/node/main.cpp`
- ช่วง 1 วินาทีแรกหลังเสียบไฟ อย่าขยับฟูก เพราะ node กำลังวัด gyro bias

### Gateway (ESP32 DevKit V1)

| อุปกรณ์ | ขา | ESP32 |
|---|---|---|
| BH1750 (GY-302) | SDA / SCL | 21 / 22 |
| DS3231 | SDA / SCL | 21 / 22 |
| DHT11 | DATA | 27 |
| KY-024 | AO / DO / **+ ต่อ 3V3 เท่านั้น** | 34 / 35 |
| KY-040 | CLK / DT / SW | 32 / 33 / 25 (stage 2) |
| KY-004 | S | 26 (stage 2) |
| KY-006 | S | 13 (stage 2) |
| MG90S | signal (ไฟ 5 V แยก + C 470 µF) | 16 (stage 2) |
| ILI9488 SPI | SCK / MOSI / CS / DC / RST | 18 / 23 / 5 / 2 / 4 (stage 2) |
| XPT2046 touch | T_CLK / T_DIN / T_DO / T_CS / T_IRQ | 18 / 23 / 19 / 15 / 36 (stage 2) |

**ILI9488:** อย่าต่อขา SDO(MISO) ของจอ ให้ต่อแค่ T_DO ของทัชเข้าขา 19 เพราะ ILI9488 ไม่ปล่อยสาย MISO ทำให้ทัชอ่านค่าไม่ได้

**ตั้งค่า Hall:** ยกถาดขึ้น (ไม่มีแม่เหล็กใกล้ sensor) แล้วพิมพ์ `c` ที่ gateway เพื่อบันทึก baseline
- พิมพ์ `h` เพื่อดูค่าดิบ
- ถ้าดันถาดลงแล้ว `phoneInserted` ไม่เปลี่ยน ให้ลด `HALL_ON_DELTA` ใน `src/gateway/config.h`

## ข้อมูลที่ส่งขึ้น NETPIE (`@shadow/data/update`)

ส่งทุก 10 วินาที 1 record (`action: "bed"`) ประกอบด้วย
- ข้อมูลจาก gateway: `tempCelsius`, `humidityPercent`, `lightIntensity`, `hallRaw`, `phoneInserted`
- ข้อมูลเตียงจาก node: `bed_*` เช่น `bed_move_count`, `bed_jerk_mg`, `bed_tof_mm` ฯลฯ
- สถิติการส่ง: `link_rx`, `link_lost`, `link_pdr` (packet delivery rate %) ใช้เป็น metric ในรายงานได้เลย

record แบบอื่น:
- `telemetry` เมื่อ node ไม่ได้ส่งข้อมูลมา
- event: `phoneInserted`, `phoneRemoved`, `nodeOnline`, `nodeOffline`, `powerOn`, `reboot`

ฝั่ง NETPIE ต้องตั้ง Device Schema ให้เก็บ field ที่ต้องการลง feed เพื่อให้ dashboard วาดกราฟย้อนหลังได้

ตอนเน็ตหลุด gateway เก็บ record ลง LittleFS (ประมาณ 2.5 ชั่วโมง) แล้วส่งย้อนหลังเมื่อต่อได้ ทุก record มี `timestamp` (epoch ms) ระดับบนสุดของ payload ตามที่ NETPIE รองรับ ข้อมูลที่ส่งย้อนหลังจึงลงจุดเวลาจริงบนกราฟ และไม่ไปทับค่าปัจจุบันใน shadow เวลาแบบอ่านง่ายอยู่ใน field `time_local`

บน NETPIE มี project `Doomclock` และอุปกรณ์ `gateway` แล้ว Device Schema ตั้งให้เก็บทุก field ลง Feed

## Root CA สำหรับ E5

ถ้าไม่ใส่ CA, TLS จะเข้ารหัสแต่ไม่ verify server หา CA ได้จาก:

```
openssl s_client -showcerts -connect mqtt.netpie.io:1884 </dev/null
openssl s_client -showcerts -connect discord.com:443 </dev/null
```

เอา certificate ตัวสุดท้ายในสาย (root/intermediate) ใส่ใน `NETPIE_ROOT_CA` / `DISCORD_ROOT_CA` ใน `secrets.h`

## งานที่ยังไม่ได้ทำ (stage 2)

- ตารางปลุก 7 วัน และเป้าหมายชั่วโมงนอน
- state machine การล็อก
- servo
- จอ TFT + ทัช
- ปุ่มเก็บ label และโหมดเก็บข้อมูล
- เสียงเตือน
- โมเดล AI
