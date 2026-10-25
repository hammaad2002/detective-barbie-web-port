/// <reference lib="webworker" />
// Writes one file into OPFS with a sync access handle (fallback for browsers without
// createWritable on the main thread). Message in: { path: string[], data: Uint8Array }.
self.onmessage = async (e: MessageEvent<{ path: string[]; data: Uint8Array }>) => {
  try {
    const { path, data } = e.data;
    let dir = await navigator.storage.getDirectory();
    for (const part of path.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create: true });
    const fh = await dir.getFileHandle(path[path.length - 1], { create: true });
    type WritableFh = FileSystemFileHandle & {
      createSyncAccessHandle?: () => Promise<FileSystemSyncAccessHandle>;
      createWritable?: () => Promise<FileSystemWritableFileStream>;
    };
    const wfh = fh as WritableFh;
    if (typeof wfh.createSyncAccessHandle === "function") {
      const h = await wfh.createSyncAccessHandle();
      h.truncate(0);
      h.write(data, { at: 0 });
      h.flush();
      h.close();
    } else if (typeof wfh.createWritable === "function") {
      const w = await wfh.createWritable();
      await w.write(data as unknown as FileSystemWriteChunkType);
      await w.close();
    } else {
      throw new Error("OPFS file writes are not supported in this browser");
    }
    postMessage({ ok: true });
  } catch (err) {
    postMessage({ ok: false, error: err instanceof Error ? err.message : String(err) });
  }
};
