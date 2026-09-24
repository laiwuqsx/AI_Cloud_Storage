import { md5Blob } from "./md5.mjs";

export const DEFAULT_CHUNK_THRESHOLD = 10 * 1024 * 1024;
export const DEFAULT_CHUNK_SIZE = 10 * 1024 * 1024;

export class UploadError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "UploadError";
    this.code = details.code;
    this.status = details.status;
    this.response = details.response;
  }
}

function normalizedBaseUrl(value) {
  return value.endsWith("/") ? value.slice(0, -1) : value;
}

async function parseResponse(response) {
  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new UploadError("server returned invalid JSON", { status: response.status });
  }
  if (!response.ok || payload.code !== 0) {
    throw new UploadError(payload.msg || "upload request failed", {
      code: payload.code,
      status: response.status,
      response: payload,
    });
  }
  return payload;
}

function validateUploadArguments(file, credentials) {
  if (!file || typeof file.slice !== "function" || !file.name || file.size <= 0) {
    throw new TypeError("a non-empty File is required");
  }
  if (!credentials?.user || !credentials?.token) {
    throw new TypeError("user and token are required");
  }
}

export function createUploadClient(options = {}) {
  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  const baseUrl = normalizedBaseUrl(options.baseUrl ?? "");
  const chunkThreshold = options.chunkThreshold ?? DEFAULT_CHUNK_THRESHOLD;
  const chunkSize = options.chunkSize ?? DEFAULT_CHUNK_SIZE;
  const hashBlockSize = options.hashBlockSize ?? 2 * 1024 * 1024;

  if (typeof fetchImpl !== "function") throw new TypeError("fetch is unavailable");
  if (!Number.isInteger(chunkThreshold) || chunkThreshold <= 0
      || !Number.isInteger(chunkSize) || chunkSize <= 0) {
    throw new TypeError("invalid upload size configuration");
  }

  async function request(path, init) {
    return parseResponse(await fetchImpl(`${baseUrl}${path}`, init));
  }

  async function hashFile(file, onProgress, signal) {
    return md5Blob(file, {
      blockSize: hashBlockSize,
      signal,
      onProgress: ({ loaded, total }) => onProgress({
        phase: "hashing",
        loaded,
        total,
        percent: Math.floor((loaded / total) * 100),
      }),
    });
  }

  async function uploadSmall(file, credentials, md5, onProgress, signal) {
    const body = new FormData();
    body.append("file", file, file.name);
    onProgress({ phase: "uploading", loaded: 0, total: file.size, percent: 0 });
    const result = await request("/api/upload", {
      method: "POST",
      headers: {
        "X-Upload-User": credentials.user,
        "X-Upload-Token": credentials.token,
        "X-Upload-MD5": md5,
        "X-Upload-Size": String(file.size),
      },
      body,
      signal,
    });
    onProgress({ phase: "completed", loaded: file.size, total: file.size, percent: 100 });
    return { ...result, mode: "single", md5 };
  }

  async function uploadChunked(file, credentials, md5, onProgress, signal) {
    const initialized = await request("/api/uploads/init", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        user: credentials.user,
        token: credentials.token,
        file_name: file.name,
        md5,
        total_size: file.size,
        chunk_size: chunkSize,
      }),
      signal,
    });
    const uploaded = new Set(initialized.uploaded_chunks ?? []);
    let uploadedBytes = 0;

    for (const index of uploaded) {
      const start = index * initialized.chunk_size;
      uploadedBytes += Math.max(0, Math.min(initialized.chunk_size, file.size - start));
    }
    for (let index = 0; index < initialized.total_chunks; index += 1) {
      if (uploaded.has(index)) continue;
      const start = index * initialized.chunk_size;
      const end = Math.min(start + initialized.chunk_size, file.size);
      const chunk = file.slice(start, end);
      const chunkMd5 = await md5Blob(chunk, { blockSize: hashBlockSize, signal });
      await request(`/api/uploads/${initialized.upload_id}/chunks/${index}`, {
        method: "PUT",
        headers: {
          "X-Upload-User": credentials.user,
          "X-Upload-Token": credentials.token,
          "X-Chunk-MD5": chunkMd5,
        },
        body: chunk,
        signal,
      });
      uploadedBytes += chunk.size;
      onProgress({
        phase: "uploading",
        uploadId: initialized.upload_id,
        chunkIndex: index,
        totalChunks: initialized.total_chunks,
        loaded: uploadedBytes,
        total: file.size,
        percent: Math.floor((uploadedBytes / file.size) * 100),
      });
    }
    onProgress({
      phase: "completing",
      uploadId: initialized.upload_id,
      loaded: file.size,
      total: file.size,
      percent: 100,
    });
    const completed = await request(`/api/uploads/${initialized.upload_id}/complete`, {
      method: "POST",
      headers: {
        "X-Upload-User": credentials.user,
        "X-Upload-Token": credentials.token,
      },
      signal,
    });
    onProgress({
      phase: "completed",
      uploadId: initialized.upload_id,
      loaded: file.size,
      total: file.size,
      percent: 100,
    });
    return { ...completed, mode: "chunked", md5, uploadId: initialized.upload_id };
  }

  return {
    async upload(file, credentials, uploadOptions = {}) {
      validateUploadArguments(file, credentials);
      const onProgress = uploadOptions.onProgress ?? (() => {});
      const signal = uploadOptions.signal;
      const md5 = await hashFile(file, onProgress, signal);
      return file.size <= chunkThreshold
        ? uploadSmall(file, credentials, md5, onProgress, signal)
        : uploadChunked(file, credentials, md5, onProgress, signal);
    },
  };
}
