import assert from "node:assert/strict";
import test from "node:test";

import { IncrementalMd5, md5Blob } from "./md5.mjs";
import { createUploadClient, UploadError } from "./upload_client.mjs";

function namedBlob(parts, name, type = "application/octet-stream") {
  const blob = new Blob(parts, { type });
  Object.defineProperty(blob, "name", { value: name });
  return blob;
}

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function memoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, value),
    removeItem: (key) => values.delete(key),
    values,
  };
}

test("incremental MD5 matches standard vectors and Blob block boundaries", async () => {
  const context = new IncrementalMd5();
  context.update(new TextEncoder().encode("hello "));
  context.update(new TextEncoder().encode("world"));
  assert.equal(context.digest(), "5eb63bbbe01eeed093cb22bb8f5acdc3");
  assert.equal(await md5Blob(new Blob(["abc"]), { blockSize: 1 }),
    "900150983cd24fb0d6963f7d28e17f72");
});

test("large files follow init, ordered chunks, progress and complete", async () => {
  const calls = [];
  const responses = [
    { code: 0, upload_id: "a".repeat(64), chunk_size: 4, total_chunks: 3,
      uploaded_chunks: [] },
    { code: 0, chunk_index: 0 },
    { code: 0, chunk_index: 1 },
    { code: 0, chunk_index: 2 },
    { code: 0, msg: "upload complete" },
  ];
  const fetchImpl = async (url, options) => {
    calls.push({ url, options });
    return jsonResponse(responses.shift());
  };
  const progress = [];
  const storage = memoryStorage();
  const client = createUploadClient({
    baseUrl: "http://files.test/",
    chunkThreshold: 5,
    chunkSize: 4,
    hashBlockSize: 3,
    fetchImpl,
    storage,
  });
  const file = namedBlob(["abcdefghij"], "demo.bin");
  const result = await client.upload(file, { user: "alice", token: "token" }, {
    onProgress: (event) => progress.push(event),
  });

  assert.equal(result.mode, "chunked");
  assert.equal(result.md5, "a925576942e94b2ef57a066101b48876");
  assert.deepEqual(calls.map(({ url }) => url), [
    "http://files.test/api/uploads/init",
    `http://files.test/api/uploads/${"a".repeat(64)}/chunks/0`,
    `http://files.test/api/uploads/${"a".repeat(64)}/chunks/1`,
    `http://files.test/api/uploads/${"a".repeat(64)}/chunks/2`,
    `http://files.test/api/uploads/${"a".repeat(64)}/complete`,
  ]);
  assert.deepEqual(calls.slice(1, 4).map(({ options }) => options.body.size), [4, 4, 2]);
  assert.deepEqual(progress.filter(({ phase }) => phase === "uploading")
    .map(({ loaded }) => loaded), [4, 8, 10]);
  assert.equal(progress.at(-1).phase, "completed");
  assert.equal(storage.values.size, 0);
});

test("saved sessions query server state and upload only missing chunks", async () => {
  const uploadId = "b".repeat(64);
  const fileMd5 = "a925576942e94b2ef57a066101b48876";
  const key = `ai-cloud-upload-v1:alice:${fileMd5}:10:demo.bin`;
  const storage = memoryStorage({
    [key]: JSON.stringify({
      uploadId,
      fileName: "demo.bin",
      fileSize: 10,
      fileMd5,
      chunkSize: 4,
      totalChunks: 3,
    }),
  });
  const calls = [];
  const responses = [
    { code: 0, file_name: "demo.bin", md5: fileMd5, total_size: 10,
      chunk_size: 4, total_chunks: 3, status: "receiving", uploaded_chunks: [0, 2] },
    { code: 0, chunk_index: 1 },
    { code: 0, msg: "upload complete" },
  ];
  const client = createUploadClient({
    chunkThreshold: 5,
    chunkSize: 4,
    hashBlockSize: 3,
    storage,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return jsonResponse(responses.shift());
    },
  });
  const result = await client.upload(namedBlob(["abcdefghij"], "demo.bin"),
    { user: "alice", token: "token" });

  assert.equal(result.resumed, true);
  assert.deepEqual(calls.map(({ url }) => url), [
    `/api/uploads/${uploadId}`,
    `/api/uploads/${uploadId}/chunks/1`,
    `/api/uploads/${uploadId}/complete`,
  ]);
  assert.equal(calls[1].options.body.size, 4);
  assert.equal(storage.values.size, 0);
});

test("expired saved sessions are replaced with a fresh initialized session", async () => {
  const oldUploadId = "c".repeat(64);
  const newUploadId = "d".repeat(64);
  const fileMd5 = "a925576942e94b2ef57a066101b48876";
  const key = `ai-cloud-upload-v1:alice:${fileMd5}:10:demo.bin`;
  const storage = memoryStorage({
    [key]: JSON.stringify({ uploadId: oldUploadId, chunkSize: 4, totalChunks: 3 }),
  });
  const calls = [];
  const responses = [
    { code: 4, msg: "chunk upload session unavailable" },
    { code: 0, upload_id: newUploadId, chunk_size: 4, total_chunks: 3,
      uploaded_chunks: [] },
    { code: 0 }, { code: 0 }, { code: 0 }, { code: 0, msg: "upload complete" },
  ];
  const client = createUploadClient({
    chunkThreshold: 5,
    chunkSize: 4,
    storage,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return jsonResponse(responses.shift());
    },
  });

  await client.upload(namedBlob(["abcdefghij"], "demo.bin"),
    { user: "alice", token: "token" });
  assert.equal(calls[0].url, `/api/uploads/${oldUploadId}`);
  assert.equal(calls[1].url, "/api/uploads/init");
  assert.equal(storage.values.size, 0);
});

test("completed sessions recover a lost complete response without re-uploading", async () => {
  const uploadId = "e".repeat(64);
  const fileMd5 = "a925576942e94b2ef57a066101b48876";
  const key = `ai-cloud-upload-v1:alice:${fileMd5}:10:demo.bin`;
  const storage = memoryStorage({
    [key]: JSON.stringify({ uploadId, chunkSize: 4, totalChunks: 3 }),
  });
  const calls = [];
  const client = createUploadClient({
    chunkThreshold: 5,
    storage,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return jsonResponse({
        code: 0,
        file_name: "demo.bin",
        md5: fileMd5,
        total_size: 10,
        chunk_size: 4,
        total_chunks: 3,
        status: "completed",
        uploaded_chunks: [0, 1, 2],
      });
    },
  });
  const result = await client.upload(namedBlob(["abcdefghij"], "demo.bin"),
    { user: "alice", token: "token" });

  assert.equal(result.msg, "upload already completed");
  assert.equal(result.resumed, true);
  assert.equal(calls.length, 1);
  assert.equal(storage.values.size, 0);
});

test("failed completion keeps the saved session for a later resume", async () => {
  const storage = memoryStorage();
  const uploadId = "f".repeat(64);
  const responses = [
    { code: 0, upload_id: uploadId, chunk_size: 4, total_chunks: 2,
      uploaded_chunks: [] },
    { code: 0 }, { code: 0 },
    { code: 7, msg: "storage upload failed" },
  ];
  const client = createUploadClient({
    chunkThreshold: 3,
    chunkSize: 4,
    storage,
    fetchImpl: async () => jsonResponse(responses.shift()),
  });

  await assert.rejects(
    client.upload(namedBlob(["abcdefgh"], "keep.bin"),
      { user: "alice", token: "token" }),
    (error) => error instanceof UploadError && error.code === 7,
  );
  assert.equal(storage.values.size, 1);
});

test("small files use the existing multipart endpoint", async () => {
  const calls = [];
  const client = createUploadClient({
    chunkThreshold: 100,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return jsonResponse({ code: 0, msg: "upload complete" });
    },
  });
  const file = namedBlob(["small"], "small.txt", "text/plain");
  const result = await client.upload(file, { user: "alice", token: "token" });

  assert.equal(result.mode, "single");
  assert.equal(calls[0].url, "/api/upload");
  assert.equal(calls[0].options.method, "POST");
  assert.ok(calls[0].options.body instanceof FormData);
  assert.equal(calls[0].options.headers["X-Upload-MD5"],
    "eb5c1399a871211c7e7ed732d15e3a8b");
});

test("server application errors are exposed to the caller", async () => {
  const client = createUploadClient({
    chunkThreshold: 100,
    fetchImpl: async () => jsonResponse({ code: 2, msg: "token error" }),
  });
  const file = namedBlob(["small"], "small.txt");

  await assert.rejects(
    client.upload(file, { user: "alice", token: "bad" }),
    (error) => error instanceof UploadError && error.code === 2
      && error.message === "token error",
  );
});
