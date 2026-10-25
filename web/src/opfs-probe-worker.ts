/// <reference lib="webworker" />
// Startup check: the engine's file system needs OPFS sync access handles in workers, which
// private windows refuse (Safari) or lack (Firefox). Posts "ok" or the error message.
self.onmessage = async () => {
  try {
    const dir = await navigator.storage.getDirectory();
    const fh = await dir.getFileHandle(".probe", { create: true });
    type WritableFh = FileSystemFileHandle & {
      createSyncAccessHandle?: () => Promise<FileSystemSyncAccessHandle>;
      createWritable?: () => Promise<FileSystemWritableFileStream>;
    };
    const wfh = fh as WritableFh;
    if (typeof wfh.createSyncAccessHandle === "function") {
      const h = await wfh.createSyncAccessHandle();
      h.write(new Uint8Array(1), { at: 0 });
      h.close();
    } else if (typeof wfh.createWritable === "function") {
      const w = await wfh.createWritable();
      await w.write(new Uint8Array(1));
      await w.close();
    } else {
      throw new Error("OPFS file writes are not supported in this browser");
    }
    await dir.removeEntry(".probe");
    postMessage("ok");
  } catch (err) {
    postMessage(err instanceof Error ? `${err.name}: ${err.message}` : String(err));
  }
};
