import { md5Blob } from "./md5.mjs";

export const DEFAULT_CHUNK_THRESHOLD = 10 * 1024 * 1024;
export const DEFAULT_CHUNK_SIZE = 10 * 1024 * 1024;
const SESSION_KEY_PREFIX = "ai-cloud-upload-v1";
const UPLOAD_ID_PATTERN = /^[0-9a-f]{64}$/;

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

function validStorage(storage) {
  return storage && typeof storage.getItem === "function"
    && typeof storage.setItem === "function"
    && typeof storage.removeItem === "function";
}

function sessionKey(file, user, md5) {
  return `${SESSION_KEY_PREFIX}:${encodeURIComponent(user)}:${md5}:${file.size}:`
    + encodeURIComponent(file.name);
}

function readSavedSession(storage, key) {
  if (!storage) return null;
  try {
    const record = JSON.parse(storage.getItem(key));
    if (!record || !UPLOAD_ID_PATTERN.test(record.uploadId)
        || !Number.isInteger(record.chunkSize) || record.chunkSize <= 0
        || !Number.isInteger(record.totalChunks) || record.totalChunks <= 0) return null;
    return record;
  } catch {
    return null;
  }
}

function saveSession(storage, key, session, file, md5) {
  if (!storage) return;
  try {
    storage.setItem(key, JSON.stringify({
      uploadId: session.upload_id,
      fileName: file.name,
      fileSize: file.size,
      fileMd5: md5,
      chunkSize: session.chunk_size,
      totalChunks: session.total_chunks,
    }));
  } catch {
    // Uploading still works when localStorage is disabled or full.
  }
}

function removeSession(storage, key) {
  if (!storage) return;
  try {
    storage.removeItem(key);
  } catch {
    // A completed server upload must not be reported as failed by local cleanup.
  }
}

function statusMatchesFile(status, file, md5) {
  return status.file_name === file.name && status.md5 === md5
    && status.total_size === file.size
    && Number.isInteger(status.chunk_size) && status.chunk_size > 0
    && Number.isInteger(status.total_chunks) && status.total_chunks > 0;
}

export function createUploadClient(options = {}) {
  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  const baseUrl = normalizedBaseUrl(options.baseUrl ?? "");
  const chunkThreshold = options.chunkThreshold ?? DEFAULT_CHUNK_THRESHOLD;
  const chunkSize = options.chunkSize ?? DEFAULT_CHUNK_SIZE;
  const hashBlockSize = options.hashBlockSize ?? 2 * 1024 * 1024;
  const storageCandidate = options.storage === undefined
    ? globalThis.localStorage
    : options.storage;
  const storage = validStorage(storageCandidate) ? storageCandidate : null;

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

  async function initializeSession(file, credentials, md5, key, signal) {
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
    if (!UPLOAD_ID_PATTERN.test(initialized.upload_id)
        || !Number.isInteger(initialized.chunk_size) || initialized.chunk_size <= 0
        || !Number.isInteger(initialized.total_chunks) || initialized.total_chunks <= 0) {
      throw new UploadError("server returned an invalid upload plan", {
        response: initialized,
      });
    }
    saveSession(storage, key, initialized, file, md5);
    return { session: initialized, resumed: false, alreadyCompleted: false };
  }

  async function resolveChunkSession(file, credentials, md5, key, signal) {
    const saved = readSavedSession(storage, key);

    if (!saved) return initializeSession(file, credentials, md5, key, signal);
    try {
      const status = await request(`/api/uploads/${saved.uploadId}`, {
        method: "GET",
        headers: {
          "X-Upload-User": credentials.user,
          "X-Upload-Token": credentials.token,
        },
        signal,
      });
      if (statusMatchesFile(status, file, md5) && status.status === "completed") {
        removeSession(storage, key);
        return {
          session: { ...status, upload_id: saved.uploadId },
          resumed: true,
          alreadyCompleted: true,
        };
      }
      if (statusMatchesFile(status, file, md5) && status.status === "receiving") {
        return {
          session: { ...status, upload_id: saved.uploadId },
          resumed: true,
          alreadyCompleted: false,
        };
      }
      removeSession(storage, key);
    } catch (error) {
      if (!(error instanceof UploadError) || error.code !== 4) throw error;
      removeSession(storage, key);
    }
    return initializeSession(file, credentials, md5, key, signal);
  }

  async function uploadChunked(file, credentials, md5, onProgress, signal) {
    const key = sessionKey(file, credentials.user, md5);
    const resolved = await resolveChunkSession(file, credentials, md5, key, signal);
    const initialized = resolved.session;
    if (resolved.alreadyCompleted) {
      onProgress({
        phase: "completed",
        uploadId: initialized.upload_id,
        loaded: file.size,
        total: file.size,
        percent: 100,
        resumed: true,
      });
      return {
        code: 0,
        msg: "upload already completed",
        mode: "chunked",
        md5,
        uploadId: initialized.upload_id,
        resumed: true,
      };
    }
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
        resumed: resolved.resumed,
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
    removeSession(storage, key);
    onProgress({
      phase: "completed",
      uploadId: initialized.upload_id,
      loaded: file.size,
      total: file.size,
      percent: 100,
      resumed: resolved.resumed,
    });
    return {
      ...completed,
      mode: "chunked",
      md5,
      uploadId: initialized.upload_id,
      resumed: resolved.resumed,
    };
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
