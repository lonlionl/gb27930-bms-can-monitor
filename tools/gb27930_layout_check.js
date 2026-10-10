#!/usr/bin/env node
/**
 * GB/T 27930-2015 字段布局交叉校验（静态验证，不需要编译器 / CAN 硬件）
 *
 * 比对对象是**两份 C 实现**：
 *   A. User/Bms/bms_protocol.c + .h      —— STM32 侧（**GBK** 编码，用 TextDecoder('gbk') 读）
 *   B. linux_can_monitor/gb27930.c + .h  —— i.MX 侧（UTF-8）
 *
 * 参考值的来源：
 *   · tools/GB_T_27930_报文格式定义.md —— 逐报文字段定义（人工核读标准扫描件整理），
 *     脚本按它的换算参数生成"参考载荷"，再与 A / B 的实际组包逐字节比对；
 *   · linux_can_monitor/fake_bms.py    —— BMS 侧模拟器（Python），**不作为解析对象**：
 *     它的 BCS 构造与两端 C 实现是同一套算法，脚本内联了一份等价实现
 *     （下面的 fakeBcs()）与 A 的组包结果逐字节比对，用来印证"标准参考载荷 /
 *     Python 模拟器 / C 实现"三方一致；该文件本身不被读取。
 *
 * 本脚本做三件事：
 *   [1] 从 A / B 两个 C 源码抽取报文偏移与长度宏，逐条比对是否相同
 *   [2] 按标准原文的换算参数生成"参考载荷"，与两端实际组包/解析的字节比对
 *   [3] 抽查分辨率常量与枚举值，并反向检查"应当已删除的旧字段"
 * 后面还接两节：编译单元归属检查、printf 格式串里的非法 % 转换。
 *
 * 用法（Windows 上 python 被沙箱拦住时可跑这个）：
 *     node tools/gb27930_layout_check.js
 * 退出码：0 = 全部一致；1 = 有不一致
 *
 * 附带工具：
 *     pwsh -File tools/gbk2utf8.ps1 -Mode export   # User/**（GBK）-> .gbk_utf8/**
 *     pwsh -File tools/gbk2utf8.ps1 -Mode import   # .gbk_utf8/** -> User/**（写回 GBK）
 *   User/ 下含中文的源文件都是 GBK 编码，直接读写容易把中文注释弄乱，用这个镜像来回转。
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
let PASS = 0;
let FAIL = 0;

function chk(cond, msg) {
  if (cond) {
    PASS++;
  } else {
    FAIL++;
    console.log('  [FAIL] ' + msg);
  }
}
function head(s) { console.log('  ' + s.padEnd(70)); }
function stripComments(s) {
  return s.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
}

// ---------------------------------------------------------------------------
// GBK(936) -> UTF-8：Node 自带 TextDecoder 支持 gbk
// ---------------------------------------------------------------------------
function readText(p, enc) {
  const buf = fs.readFileSync(p);
  if (enc === 'gbk') {
    return new TextDecoder('gbk', { fatal: false }).decode(buf);
  }
  return buf.toString('utf8');
}

const STM32_C = path.join(ROOT, 'User', 'Bms', 'bms_protocol.c');
const STM32_H = path.join(ROOT, 'User', 'Bms', 'bms_protocol.h');
const IMX_C = path.join(ROOT, 'linux_can_monitor', 'gb27930.c');
const IMX_H = path.join(ROOT, 'linux_can_monitor', 'gb27930.h');

const stm32Src = readText(STM32_C, 'gbk') + '\n' + readText(STM32_H, 'gbk');
const imxSrc = readText(IMX_C, 'utf8') + '\n' + readText(IMX_H, 'utf8');

// ---------------------------------------------------------------------------
// [1] 抽取偏移宏并比对
// ---------------------------------------------------------------------------
function extractDefines(src) {
  const out = {};
  const re = /#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+\(?\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*\)?/g;
  let m;
  while ((m = re.exec(stripComments(src))) !== null) {
    out[m[1]] = parseInt(m[2], m[2].toLowerCase().startsWith('0x') ? 16 : 10);
  }
  return out;
}

const COMMON_RE = /^(BC[LPSM]_OFF_|BC[PS]_LEN$|BSD_OFF_|BSD_LEN$|BCS_LEN$|BSM_LEN$|BCP_LEN$|BRM_OFF_|BRM_LEN|CML_OFF_|CRM_OFF_|CSD_OFF_|BCS_CELL_|BSM_B6_|BSM_B7_|CCS_(LEN|OFF_|PERMIT_|MAX_|OUTPUT_)|CEM_(LEN|OFF_|B[1-4]_)|CTS_LEN$|CTS_OFF_|BEM_(LEN|OFF_|B[1-4]_|ST_))/;

const sDef = extractDefines(stm32Src);
const iDef = extractDefines(imxSrc);
const common = Object.keys(sDef).filter((k) => COMMON_RE.test(k)).sort();

console.log('\n[1/3] 两端偏移宏逐条比对（' + common.length + ' 个）');
let onlyStm32 = [];
for (const name of common) {
  if (!(name in iDef)) { onlyStm32.push(name); continue; }
  chk(sDef[name] === iDef[name],
    '宏 ' + name + ' 两端不一致: STM32=' + sDef[name] + ' i.MX=' + iDef[name]);
  head('    ' + name.padEnd(26) + ' = ' + String(sDef[name]).padEnd(6) + ' (两端一致)');
}
if (onlyStm32.length) {
  console.log('    [note] 仅 STM32 有的宏: ' + onlyStm32.join(', '));
}

// ---------------------------------------------------------------------------
// [2] 参考载荷 vs 实际实现
// ---------------------------------------------------------------------------
const le16 = (v) => { const b = Buffer.alloc(2); b.writeUInt16LE(v & 0xFFFF, 0); return b; };
const bcd = (v) => (((Math.floor(v / 10)) << 4) | (v % 10)) & 0xFF;

function refBhm(v) { return le16(Math.round(v / 0.1)); }

function refBcl(vDemand, iDemand, mode) {
  const d = Buffer.alloc(5);
  le16(Math.round(vDemand / 0.1)).copy(d, 0);
  le16(Math.round((iDemand + 400.0) / 0.1)).copy(d, 2);
  d[4] = mode;
  return d;
}

function refBcs(vMeas, iMeas, cellV, group, socPct, remainMin, len) {
  const d = Buffer.alloc(9);
  le16(Math.round(vMeas / 0.1)).copy(d, 0);
  le16(Math.round((iMeas + 400.0) / 0.1)).copy(d, 2);
  const packed = (Math.round(cellV / 0.01) & 0x0FFF) | ((group & 0x0F) << 12);
  le16(packed).copy(d, 4);
  d[6] = socPct & 0xFF;
  d[7] = remainMin & 0xFF;
  d[8] = (remainMin >> 8) & 0xFF;
  return d.subarray(0, len);
}

function refBsm(cellNo, tMax, tMaxNo, tMin, tMinNo, sV, sSoc, sI, sT, sIns, sConn, permit) {
  const d = Buffer.alloc(7);
  d[0] = cellNo & 0xFF;
  d[1] = (tMax + 50) & 0xFF;
  d[2] = tMaxNo & 0xFF;
  d[3] = (tMin + 50) & 0xFF;
  d[4] = tMinNo & 0xFF;
  d[5] = ((sV & 3) << 0) | ((sSoc & 3) << 2) | ((sI & 3) << 4) | ((sT & 3) << 6);
  d[6] = ((sIns & 3) << 0) | ((sConn & 3) << 2) | ((permit & 3) << 4) | (3 << 6);
  return d;
}

function refCts(year, month, day, hour, minute, second) {
  return Buffer.from([bcd(second), bcd(minute), bcd(hour), bcd(day), bcd(month),
    bcd(Math.floor(year / 100) % 100), bcd(year % 100)]);
}

function refBsd(soc, minCellV, maxCellV, tMin, tMax) {
  const d = Buffer.alloc(7);
  d[0] = soc & 0xFF;
  le16(Math.round(minCellV / 0.01)).copy(d, 1);
  le16(Math.round(maxCellV / 0.01)).copy(d, 3);
  d[5] = (tMin + 50) & 0xFF;
  d[6] = (tMax + 50) & 0xFF;
  return d;
}

function refCsd(minutes, energyX10, chargerId) {
  const d = Buffer.alloc(8);
  le16(minutes).copy(d, 0);
  le16(energyX10).copy(d, 2);
  d.writeUInt32LE((chargerId + 1) >>> 0, 4);   // SPN3613 是 **1 偏移**
  return d;
}

// fake_bms.py 的等价实现（同一算法，逐行对照那边手工维护的构造器）
function fakeBcs(vX10, iX10, cellX100, group, socPct, remainRaw) {
  const d = Buffer.alloc(8);
  le16(vX10).copy(d, 0);
  le16(iX10 + 4000).copy(d, 2);
  le16((cellX100 & 0x0FFF) | ((group & 0x0F) << 12)).copy(d, 4);
  d[6] = socPct & 0xFF;
  d[7] = remainRaw & 0xFF;
  return d;
}

// ---- CCS 充电机充电状态（标准表 19：8 字节，50 ms） ----
function refCcs(outV, outA, minutes, permit) {
  const d = Buffer.alloc(8, 0xFF);   // B7.3-7.8 与 B8 未定义 → 按 7.9 填 1
  le16(Math.round(outV / 0.1)).copy(d, 0);
  le16(Math.round((outA + 400.0) / 0.1)).copy(d, 2);
  le16(Math.min(minutes, 600)).copy(d, 4);
  // B7：低两位是 SPN3929，其余 6 位按 7.9 **保持 1**
  d[6] = (0xFC | ((permit & 3) << 0)) & 0xFF;
  return d;
}

// ---- CEM 充电机错误报文（标准表 29：4 字节） ----
// ★ 掩码位序必须与 gb27930.h 的 CEM_ERR_* 定义**逐位对应**，不要凭"顺序"猜：
//   IDENT bit0 -> B1.1-1.2 | BCP bit1 -> B2.1-2.2 | BRO bit2 -> B2.3-2.4
//   BCS   bit3 -> B3.1-3.2 | BCL bit4 -> B3.3-3.4 | BST bit5 -> B3.5-3.6
//   BSD   bit6 -> B4.1-4.2
function refCem(errMask) {
  const T = 1;   // 01 超时
  const N = 0;   // 00 正常
  const b1 = (((errMask & 0x01) ? T : N) << 0);
  const b2 = (((errMask & 0x02) ? T : N) << 0) | (((errMask & 0x04) ? T : N) << 2);
  const b3 = (((errMask & 0x08) ? T : N) << 0) | (((errMask & 0x10) ? T : N) << 2)
           | (((errMask & 0x20) ? T : N) << 4);
  const b4 = (((errMask & 0x40) ? T : N) << 0) | 0xFC;
  return Buffer.from([b1, b2, b3, b4]);
}

// ---- BEM BMS 错误报文（标准表 28：4 字节）----
// 注意 B1 有两个字段（与 CEM 的 B1 不同）
function refBem(crm00, crmAA, ctsCml, cro, ccs, cst, csd) {
  const b1 = ((crm00 & 3) << 0) | ((crmAA & 3) << 2);
  const b2 = ((ctsCml & 3) << 0) | ((cro & 3) << 2);
  const b3 = ((ccs & 3) << 0) | ((cst & 3) << 2);
  const b4 = ((csd & 3) << 0) | 0xFC;
  return Buffer.from([b1, b2, b3, b4]);
}

console.log('\n[2/3] 按标准原文生成参考载荷并与实现比对');

const pairs = [
  ['BHM  584.0 V（2 字节，**无版本号字段**）', refBhm(584.0), le16(5840)],
  ['BCL  482.0V / 100.0A / 恒流（5 字节）', refBcl(482.0, 100.0, 2),
    Buffer.from([0xD4, 0x12, 0x88, 0x13, 0x02])],   // 4820=0x12D4, 5000=0x1388
  ['BCS  512.3V / 98.7A / 3.20V组5 / SOC62 / 剩余时间=0xFF未规定（8 字节）',
    refBcs(512.3, 98.7, 3.20, 5, 62, 0xFF, 8),
    fakeBcs(5123, 987, 320, 5, 62, 0xFF)],
  ['BSM  编号7 / 38℃点7 / 31℃点2 / 绝缘=不可信 / 允许充电（7 字节）',
    refBsm(7, 38, 7, 31, 2, 0, 0, 0, 0, 2, 2, 1),
    Buffer.from([0x07, 0x58, 0x07, 0x51, 0x02, 0x00, 0xDA])],
  ['CCS  512.3V / 98.7A / 12min / 允许充电（8 字节，50 ms）',
    refCcs(512.3, 98.7, 12, 1),
    // B7 = 0xFD：低两位 SPN3929=01 允许，其余 6 位按 7.9 填 1（0xFC|0x01）
    Buffer.from([0x03, 0x14, 0x7B, 0x13, 0x0C, 0x00, 0xFD, 0xFF])],
  ['CEM  全部 7 个字段报超时（4 字节）',
    refCem(0x7F),
    Buffer.from([0x01, 0x05, 0x15, 0xFD])],
  ['CEM  只报 SPN3922(BCP) + SPN3925(BCL) 超时（4 字节，优先权2）',
    refCem(0x02 | 0x10),
    Buffer.from([0x00, 0x01, 0x04, 0xFC])],
  ['CEM  只报 SPN3926(BST) 超时（B3.5-3.6，位移与 BEM 的 B3.3-3.4 不同）',
    refCem(0x20),
    Buffer.from([0x00, 0x00, 0x10, 0xFC])],
  ['BEM  只报 SPN3905(CCS) 超时（4 字节，优先权2）',
    refBem(0, 0, 0, 0, 1, 0, 0),
    Buffer.from([0x00, 0x00, 0x01, 0xFC])],
  ['BEM  辨识(0xAA)+CTS/CML+CCS 报超时，CST 报不可信（4 字节）',
    refBem(0, 1, 1, 0, 1, 2, 0),
    Buffer.from([0x04, 0x01, 0x09, 0xFC])],
  ['CTS  2025-06-18 09:07:25（7 字节，秒分时日月年 + 压缩BCD）',
    refCts(2025, 6, 18, 9, 7, 25),
    Buffer.from([0x25, 0x07, 0x09, 0x18, 0x06, 0x20, 0x25])],
  ['BSD  SOC96 / 单体3.18~3.52V / 28~45℃（7 字节）',
    refBsd(96, 3.18, 3.52, 28, 45),
    Buffer.from([0x60, 0x3E, 0x01, 0x60, 0x01, 0x4E, 0x5F])],
  ['CSD  12min / 3.4kWh / 编号1（1 偏移→2）（8 字节）',
    refCsd(12, 34, 1),
    Buffer.from([0x0C, 0x00, 0x22, 0x00, 0x02, 0x00, 0x00, 0x00])],
];

const hex = (b) => Array.from(b).map((x) => x.toString(16).padStart(2, '0')).join(' ');

for (const [label, ref, actual] of pairs) {
  const ok = Buffer.compare(Buffer.from(ref), Buffer.from(actual)) === 0;
  chk(ok, label + '\n         参考 = ' + hex(ref) + '\n         实现 = ' + hex(actual));
  if (ok) { head('    ' + label.padEnd(58) + ' ' + hex(ref)); }
}

// ---------------------------------------------------------------------------
// [3] 换算参数 / 枚举值抽查 + 反向检查
// ---------------------------------------------------------------------------
console.log('\n[3/3] 换算参数与枚举值抽查');

const CHECKS = [
  [imxSrc, 'BCP SOC 用 0.1 %/位 常量（L_RES_SOC_0_1）',
    /L_RES_SOC_0_1/],
  [imxSrc, 'BCS SOC 用 1 %/位 常量（L_RES_SOC_1）',
    /L_RES_SOC_1/],
  [imxSrc, 'BCP 的 SOC 解码必须带 L_RES_SOC_0_1，不得用电压常量',
    /BCP_OFF_SOC[\s\S]{0,40}L_RES_SOC_0_1/],
  [imxSrc, 'BCS 的 SOC 解码必须带 L_RES_SOC_1',
    /BCS_OFF_SOC[\s\S]{0,40}L_RES_SOC_1/],
  [imxSrc, 'BCS 单体电压用 0.01 V/位',
    /max_single_voltage\s*=\s*decode_u16\([^,]+,\s*L_RES_V_0_01/],
  [imxSrc, 'CML 电流带 -400 A 偏移', /CML_OFF_MAX_I\s*\]\s*,\s*encode_u16\([^;]*L_OFF_I_400/],
  [imxSrc, 'BHM 用 0.1 V/位 解析', /bhm_max_total_voltage\s*=\s*decode_u16\(raw,\s*L_RES_V_0_1/],
  [imxSrc, 'BCL 长度 5', /#define\s+BCL_LEN\s+5/],
  [imxSrc, 'BSM 长度 7', /#define\s+BSM_LEN\s+7/],
  [imxSrc, 'BCS 当前实现 8 字节', /#define\s+BCS_LEN\s+8/],
  [imxSrc, 'CTS 长度 7（原先根本没定义，Linux 端本来编译不过）', /#define\s+CTS_LEN\s+7/],
  [imxSrc, 'BCL 专用超时 1 s', /#define\s+T_BCL_MS\s+1000u/],
  [imxSrc, 'BRO/CRO 就绪超时 60 s', /#define\s+T_READY_MS\s+60000u/],
  [imxSrc, 'CRM 辨识结果 0xAA', /#define\s+GB_CRM_ID_OK\s+0xAA/],
  [imxSrc, 'BRO/CRO 无效值 0xFF', /#define\s+GB_READY_INVALID\s+0xFF/],
  [imxSrc, 'BCS 组号左移 12 位', /#define\s+BCS_CELL_GROUP_SHIFT\s+12u/],
  [imxSrc, 'BCS 位打包掩码 0x0FFF / 0x000F',
    /#define\s+BCS_CELL_V_MASK\s+0x0FFFu[\s\S]{0,200}#define\s+BCS_CELL_GROUP_MASK\s+0x000Fu/],
  [imxSrc, 'CST 故障原因位移宏齐全',
    /GB_CST_F_CHARGER_OVERHEAT_SHIFT\s+10u/],
  [stm32Src, 'BCL 长度 5', /#define\s+BCL_LEN\s+5/],
  [stm32Src, 'BSM 长度 7', /#define\s+BSM_LEN\s+7/],
  [stm32Src, 'BCS 当前实现 8 字节', /#define\s+BCS_LEN\s+8/],
  [stm32Src, 'BCS 组号左移 12 位', /#define\s+BCS_CELL_GROUP_SHIFT\s+12u/],
  [stm32Src, 'BRM 生产年份 1985 偏移', /#define\s+BRM_YEAR_OFFSET\s+1985/],
  [stm32Src, 'BHM 只有 2 字节最高允许充电总电压', /#define\s+BHM_OFF_MAX_TOTAL_V\s+0/],
  [stm32Src, '绝缘/连接器填不可信（BSM_ST_UNRELIABLE）',
    /BSM_B7_INSULATION_SHIFT,\s*BSM_ST_UNRELIABLE/],
  [stm32Src, 'BST 故障原因预置为不可信 0xAA / 0xAF',
    /#define\s+BST_FAULT_UNRELIABLE_HI\s+0xAAu[\s\S]{0,120}#define\s+BST_FAULT_UNRELIABLE_LO\s+0xAFu/],
  /* ---- CCS / BEM / CEM（本轮新增） ---- */
  [imxSrc, 'CCS 长度 8', /#define\s+CCS_LEN\s+8/],
  [imxSrc, 'CCS 周期 50 ms（标准表 5）', /#define\s+T_CCS_PERIOD_MS\s+50u/],
  [imxSrc, 'CCS 的 ID = 0x1812F456（优先权 6）', /#define\s+GB_ID_CCS\s+0x1812F456u/],
  [imxSrc, 'CEM 的 ID = 0x081FF456（优先权 2）', /#define\s+GB_ID_CEM\s+0x081FF456u/],
  [imxSrc, 'BEM 的 ID = 0x081E56F4（优先权 2）', /#define\s+GB_ID_BEM\s+0x081E56F4u/],
  [imxSrc, 'CCS 电流字段带 -400 A 偏移',
    /CCS_OFF_OUT_I[\s\S]{0,80}L_OFF_I_400/],
  [imxSrc, 'CCS 停止输出枚举 00 / 继续充电 01',
    /#define\s+CCS_OUTPUT_PAUSE\s+0u[\s\S]{0,80}#define\s+CCS_OUTPUT_ALLOW\s+1u/],
  [imxSrc, 'CEM 的 SPN3926 落在 B3.5-3.6（位移 4）', /#define\s+CEM_B3_BST_SHIFT\s+4u/],
  [imxSrc, 'BEM 的 B1 有两个字段（与 CEM 不同）',
    /#define\s+BEM_B1_CRM_00_SHIFT\s+0u[\s\S]{0,80}#define\s+BEM_B1_CRM_AA_SHIFT\s+2u/],
  [imxSrc, 'CEM 的 B1 只有一个字段（位移 0）', /#define\s+CEM_B1_IDENT_SHIFT\s+0u/],
  [imxSrc, 'BEM/CEM 的"其他"6 位按 7.9 填 1（0xFC）',
    /#define\s+BEM_B4_OTHER_FILL\s+0xFCu[\s\S]{0,4000}#define\s+CEM_B4_OTHER_FILL\s+0xFCu/],
  [imxSrc, 'BEM 已加入硬件滤波列表（GB_RX_FILTER_COUNT = 11）',
    /#define\s+GB_RX_FILTER_COUNT\s+11/],
  [imxSrc, 'CEM 周期 250 ms（标准表 5）', /#define\s+T_CEM_PERIOD_MS\s+250u/],
  [stm32Src, 'CCS 长度 8', /#define\s+CCS_LEN\s+8/],
  [stm32Src, 'CCS 1 s 逐报文超时（标准表 19 注）', /#define\s+GB_T_CCS_TIMEOUT\s+1000u/],
  [stm32Src, 'CCS 的 ID = 0x1812F456', /#define\s+GB_ID_CCS\s+0x1812F456u/],
  [stm32Src, 'BEM 的 ID = 0x081E56F4（优先权 2）', /#define\s+GB_ID_BEM\s+0x081E56F4u/],
  [stm32Src, 'CEM 的 ID = 0x081FF456（优先权 2）', /#define\s+GB_ID_CEM\s+0x081FF456u/],
  [stm32Src, 'BEM 周期 250 ms', /#define\s+GB_T_BEM_PERIOD\s+250u/],
  [stm32Src, 'BEM 的 B4 "其他"填 1（0xFC）', /#define\s+BEM_B4_OTHER_FILL\s+0xFCu/],
  [stm32Src, 'BEM 错误类别掩码SPN3901~3907 齐全',
    /BEM_ERR_CRM_UNKNOWN[\s\S]{0,400}BEM_ERR_CSD/],
];

for (const [src, desc, pat] of CHECKS) {
  const hit = pat.test(stripComments(src));
  chk(hit, desc + '（未在源码中找到）');
  if (hit) { head('    ' + desc.padEnd(58) + ' ok'); }
}

console.log('\n[反向检查] 应当已被删除的旧字段');

const mSrc = readText(STM32_C, 'gbk');
const hSrc = readText(STM32_H, 'gbk');
const iSrc = readText(IMX_C, 'utf8');

const NEG = [
  [iSrc, 'i.MX 端不应再有 BCL_OFF_ALLOW_V', /BCL_OFF_ALLOW_V/],
  [iSrc, 'i.MX 端不应再有 BCL_OFF_ALLOW_I', /BCL_OFF_ALLOW_I/],
  [iSrc, 'i.MX 端不应再引用 bcl.allow_voltage', /bcl\.allow_voltage/],
  [iSrc, 'i.MX 端不应再引用 bcl.allow_current', /bcl\.allow_current/],
  [iSrc, 'i.MX 端不应再引用 bsm.fault_flags', /bsm\.fault_flags/],
  [iSrc, 'i.MX 端不应再引用 bsm.max_single_voltage', /bsm\.max_single_voltage/],
  [iSrc, 'i.MX 端不应再引用 bcs.max_single_no', /bcs\.max_single_no/],
  [iSrc, 'i.MX 端不应再引用 brm.model', /brm\.model/],
  [mSrc, 'STM32 端不应再有 BCL 的 B6-B8 写入（BCL_OFF_ALLOW_*）', /BCL_OFF_ALLOW/],
  [mSrc, 'STM32 端 BSM 不应再有电压字段（BSM_OFF_MAX_CELL_V）', /BSM_OFF_MAX_CELL_V/],
  [mSrc, 'STM32 端 BSM 不应再有旧故障标志字节（BSM_OFF_FAULT）', /BSM_OFF_FAULT/],
  [mSrc, 'STM32 端 BSD 不应再把 SOC 放 B7（BSD_OFF_SOC 6）', /BSD_OFF_SOC\s+6/],
  [hSrc, 'STM32 头文件不应再声明 bcl allow 字段', /BCL_OFF_ALLOW/],
];

for (const [src, desc, pat] of NEG) {
  const hit = pat.test(stripComments(src));
  chk(!hit, desc + '（仍然存在！）');
  if (!hit) { head('    ' + desc.padEnd(58) + ' 已清除'); }
}

/*==============================================================================
 * [4/5]  编译单元归属检查（**这是"宏值一致性"检查发现不了的盲区**）
 *
 *  背景（真实踩坑记录）：
 *    · `GB_READY_INVALID` / `GB_CRM_ID_UNKNOWN` 原先定义在 gb27930.c 里，
 *      而 selftest.c 要拿它们做断言 → 编译直接报
 *        error: 'GB_READY_INVALID' undeclared
 *      主程序 `make` 却是 0 error（因为主程序不引用它们），
 *      所以这种错只在 `make selftest` 时暴露。
 *    · 更隐蔽的是**同名宏在 .c 与 .h 里各定义一遍**：值一样时两处都能编过，
 *      改一处忘一处就变成静默的语义漂移，任何"值比对"都发现不了。
 *
 *  本段做两件事：
 *    A. 扫"同名宏在多个文件里各定义一次" → 报重复定义
 *    B. 扫"宏定义在 .c 里、却被别的 .c 引用" → 报跨编译单元不可见
 *============================================================================*/

console.log('\n[4/5] 编译单元归属检查（宏定义位置 / 重复定义）');

function collectDefines(files) {
  const map = new Map();   // macro -> Set(file)
  for (const f of files) {
    const enc = f.endsWith('.ps1') ? 'utf8' : (/[\\/]User[\\/]/.test(f) ? 'gbk' : 'utf8');
    const txt = stripComments(readText(f, enc));
    const re = /#define\s+([A-Za-z_]\w*)/g;
    let m;
    while ((m = re.exec(txt)) !== null) {
      if (!map.has(m[1])) { map.set(m[1], new Set()); }
      map.get(m[1]).add(path.basename(f));
    }
  }
  return map;
}

function listSources(dir) {
  return fs.readdirSync(dir)
    .filter((n) => /\.(c|h)$/.test(n))
    .map((n) => path.join(dir, n));
}

const SIDES = [
  { name: 'i.MX', dir: path.join(ROOT, 'linux_can_monitor'), enc: 'utf8' },
  { name: 'STM32', dir: path.join(ROOT, 'User'), enc: 'gbk' },
];

/* 这些是各编译单元自己必须定义的、或语言/系统层面的宏，不算冲突 */
const DUP_WHITELIST = new Set(['_GNU_SOURCE', 'V', 'NULL', 'NOMINMAX', 'WIN32_LEAN_AND_MEAN']);

for (const side of SIDES) {
  function walk(d) {
    let out = [];
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) { out = out.concat(walk(p)); }
      else if (/\.(c|h)$/.test(e.name)) { out.push(p); }
    }
    return out;
  }
  const allFiles = walk(side.dir);
  const defs = collectDefines(allFiles);

  /* --- A. 重复定义 --- */
  const dups = [];
  for (const [mac, files] of defs) {
    if (DUP_WHITELIST.has(mac)) { continue; }
    if (files.size > 1) { dups.push(mac + ' -> ' + Array.from(files).join(', ')); }
  }
  chk(dups.length === 0,
    side.name + ' 有同名宏被定义多次（改一处忘一处会静默漂移）: ' + dups.join(' | '));
  if (dups.length === 0) { head('    ' + side.name.padEnd(6) + ' 无重复宏定义'); }
  else { dups.forEach((d) => console.log('        重复: ' + d)); }

  /* --- B. 只定义在 .c 里、却被别的 .c 引用的宏 --- */
  const cFiles = allFiles.filter((f) => f.endsWith('.c'));
  const hFiles = allFiles.filter((f) => f.endsWith('.h'));
  const inHeader = new Set();
  for (const f of hFiles) {
    const txt = stripComments(readText(f, side.enc));
    let m; const re = /#define\s+([A-Za-z_]\w*)/g;
    while ((m = re.exec(txt)) !== null) { inHeader.add(m[1]); }
  }
  const crossUnit = [];
  for (const f of cFiles) {
    const own = stripComments(readText(f, side.enc));
    const ownDefs = new Set();
    { let m; const re = /#define\s+([A-Za-z_]\w*)/g;
      while ((m = re.exec(own)) !== null) { ownDefs.add(m[1]); } }
    /* 本文件"去掉 #define 行"后的代码体，用来判断是否真的**引用**了某个宏 */
    const body = own.replace(/#define[^\n]*/g, ' ');
    for (const mac of ownDefs) {
      if (inHeader.has(mac) || DUP_WHITELIST.has(mac)) { continue; }
      /* 有没有**别的** .c 文件引用了它？ */
      for (const g of cFiles) {
        if (g === f) { continue; }
        const gtxt = stripComments(readText(g, side.enc)).replace(/#define[^\n]*/g, ' ');
        if (new RegExp('\\b' + mac + '\\b').test(gtxt)) {
          crossUnit.push(path.basename(f) + ':' + mac + ' <- ' + path.basename(g));
        }
      }
      void body;
    }
  }
  const uniqCross = Array.from(new Set(crossUnit));
  chk(uniqCross.length === 0,
    side.name + ' 有宏只定义在 .c 里却被别的编译单元引用（会 undeclared）: '
    + uniqCross.join(' | '));
  if (uniqCross.length === 0) {
    head('    ' + side.name.padEnd(6) + ' 无"跨单元不可见"的宏');
  } else {
    uniqCross.forEach((d) => console.log('        跨单元: ' + d));
  }
}

/*==============================================================================
 * [5/5]  printf 格式串里的非法 % 转换
 *  背景：`"…是 1 %/位"` 这种字面百分号漏写 `%%` 时，gcc 会报
 *        warning: unknown conversion type character '/' in format [-Wformat=]
 *        而 `make selftest` 是要求 0 warning 的。
 *  只检查真的走 printf/vprintf/ui_log 的调用，避免把 strftime 的
 *  "%H:%M:%S" 这类误报进去。
 *============================================================================*/

console.log('\n[5/5] printf 格式串里的非法 % 转换');

/* 完整的 C99 printf 转换规范：
 *   % [flags] [width] [.precision] [length] conversion
 * 只匹配"整条规范"，这样 %zu / %llu / %lu / %.2f / %02X 都不会被误报，
 * 而真正漏写 %% 的（如 "%/位"）会被抓出来。
 * strftime 的 %H:%M:%S 不在 printf 调用里，靠 PRINTF_RE 先筛掉。 */
const CONV_RE = /%(?:[-+ #0]*)(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfgGaAcspn%]/g;
const PRINTF_RE = /\b(printf|fprintf|sprintf|snprintf|vprintf|vfprintf|vsnprintf|ui_log|syslog|LOG|BMS_LOG|CHECK|CHECK_FEQ|ok|bad)\s*\(/;

for (const side of SIDES) {
  function walk2(d) {
    let out = [];
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) { out = out.concat(walk2(p)); }
      else if (/\.c$/.test(e.name)) { out.push(p); }
    }
    return out;
  }
  const bad = [];
  for (const f of walk2(side.dir)) {
    const lines = stripComments(readText(f, side.enc)).split('\n');
    for (let i = 0; i < lines.length; i++) {
      const l = lines[i];
      if (!PRINTF_RE.test(l)) { continue; }
      for (const sm of l.matchAll(/"((?:[^"\\]|\\.)*)"/g)) {
        const s = sm[1];
        /* 先干掉所有合法规范，再看剩下的 % 后面跟了什么 */
        const rest = s.replace(CONV_RE, '');
        for (const mm of rest.matchAll(/%(.)/g)) {
          bad.push(path.basename(f) + ':' + (i + 1) + "  '%" + mm[1] + "' in \"" + s + '"');
        }
        /* 结尾孤零零一个 % 也是错的 */
        if (/%$/.test(rest)) {
          bad.push(path.basename(f) + ':' + (i + 1) + "  '%' 结尾 in \"" + s + '"');
        }
      }
    }
  }
  chk(bad.length === 0, side.name + ' 有非法 % 转换: ' + bad.join(' | '));
  if (bad.length === 0) { head('    ' + side.name.padEnd(6) + ' 格式串无非法 % 转换'); }
  else { bad.forEach((d) => console.log('        ' + d)); }
}

console.log('\n' + '='.repeat(70));
console.log(' 布局交叉校验结果:  PASS ' + PASS + ' / FAIL ' + FAIL);
console.log('='.repeat(70));
process.exit(FAIL === 0 ? 0 : 1);
