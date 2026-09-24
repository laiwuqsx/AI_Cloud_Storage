const ROTATIONS = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

const CONSTANTS = [
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
  0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
  0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
  0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
  0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
  0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
  0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
  0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
];

function rotateLeft(value, amount) {
  return (value << amount) | (value >>> (32 - amount));
}

export class IncrementalMd5 {
  constructor() {
    this.state = new Uint32Array([
      0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
    ]);
    this.buffer = new Uint8Array(64);
    this.bufferLength = 0;
    this.byteLength = 0;
    this.finished = false;
  }

  update(input) {
    if (this.finished) throw new Error("MD5 context is already finalized");
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    let offset = 0;

    this.byteLength += bytes.length;
    if (this.bufferLength > 0) {
      const count = Math.min(64 - this.bufferLength, bytes.length);
      this.buffer.set(bytes.subarray(0, count), this.bufferLength);
      this.bufferLength += count;
      offset += count;
      if (this.bufferLength === 64) {
        this.#transform(this.buffer);
        this.bufferLength = 0;
      }
    }
    while (offset + 64 <= bytes.length) {
      this.#transform(bytes.subarray(offset, offset + 64));
      offset += 64;
    }
    if (offset < bytes.length) {
      this.buffer.set(bytes.subarray(offset), 0);
      this.bufferLength = bytes.length - offset;
    }
    return this;
  }

  digest() {
    if (this.finished) throw new Error("MD5 context is already finalized");
    const originalBits = BigInt(this.byteLength) * 8n;
    const paddingLength = this.bufferLength < 56
      ? 56 - this.bufferLength
      : 120 - this.bufferLength;
    const padding = new Uint8Array(paddingLength);
    padding[0] = 0x80;
    this.update(padding);
    const lengthBytes = new Uint8Array(8);
    for (let index = 0; index < 8; index += 1) {
      lengthBytes[index] = Number((originalBits >> BigInt(index * 8)) & 0xffn);
    }
    this.update(lengthBytes);
    this.finished = true;

    let output = "";
    for (const word of this.state) {
      for (let shift = 0; shift < 32; shift += 8) {
        output += ((word >>> shift) & 0xff).toString(16).padStart(2, "0");
      }
    }
    return output;
  }

  #transform(block) {
    const words = new Uint32Array(16);
    for (let index = 0; index < 16; index += 1) {
      const offset = index * 4;
      words[index] = block[offset]
        | (block[offset + 1] << 8)
        | (block[offset + 2] << 16)
        | (block[offset + 3] << 24);
    }

    let a = this.state[0] | 0;
    let b = this.state[1] | 0;
    let c = this.state[2] | 0;
    let d = this.state[3] | 0;
    for (let index = 0; index < 64; index += 1) {
      let value;
      let wordIndex;
      if (index < 16) {
        value = (b & c) | (~b & d);
        wordIndex = index;
      } else if (index < 32) {
        value = (d & b) | (~d & c);
        wordIndex = (5 * index + 1) % 16;
      } else if (index < 48) {
        value = b ^ c ^ d;
        wordIndex = (3 * index + 5) % 16;
      } else {
        value = c ^ (b | ~d);
        wordIndex = (7 * index) % 16;
      }
      const previousD = d;
      d = c;
      c = b;
      const sum = (a + value + CONSTANTS[index] + words[wordIndex]) | 0;
      b = (b + rotateLeft(sum, ROTATIONS[index])) | 0;
      a = previousD;
    }
    this.state[0] = (this.state[0] + a) >>> 0;
    this.state[1] = (this.state[1] + b) >>> 0;
    this.state[2] = (this.state[2] + c) >>> 0;
    this.state[3] = (this.state[3] + d) >>> 0;
  }
}

export async function md5Blob(blob, options = {}) {
  const blockSize = options.blockSize ?? 2 * 1024 * 1024;
  const onProgress = options.onProgress ?? (() => {});
  const signal = options.signal;
  const context = new IncrementalMd5();

  if (!blob || typeof blob.slice !== "function" || !Number.isInteger(blockSize)
      || blockSize <= 0) throw new TypeError("invalid Blob or MD5 block size");
  for (let offset = 0; offset < blob.size; offset += blockSize) {
    if (signal?.aborted) throw new DOMException("Upload aborted", "AbortError");
    const end = Math.min(offset + blockSize, blob.size);
    context.update(new Uint8Array(await blob.slice(offset, end).arrayBuffer()));
    onProgress({ loaded: end, total: blob.size });
  }
  return context.digest();
}
