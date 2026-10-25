// Shared helpers for the bench scripts. They run inside the bench container (see ../Dockerfile):
// /bench is this folder, /data the bench's volume (sets and results), /downloads the raw
// downloads (read-only), /private FynePDF's own test files (read-only, never published).

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";

export const DATA = process.env.BENCH_DATA || "/data";
export const SETS = path.join(DATA, "sets");
export const RESULTS = path.join(DATA, "results");
export const MANIFEST = path.join(DATA, "manifest.json");

// The container's npm libraries live in /opt/deps, outside the mounted folder.
export const requireDep = createRequire("/opt/deps/package.json");
export const pdfjsPath = "/opt/deps/node_modules/pdfjs-dist/legacy/build/pdf.mjs";

const CAP = 64 * 1024;

/**
 * Runs a program with a time limit. With |measure|, GNU time reports its wall seconds and peak
 * memory (the largest resident set of the program and its children). A run past the limit is
 * killed with its whole process group, so a converter's helpers die with it.
 */
export function run(cmd, args, { timeoutMs = 180_000, cwd, env, measure = false } = {}) {
  return new Promise((resolve) => {
    const timeFile = measure ? path.join(os.tmpdir(), `time-${process.pid}-${Math.random().toString(36).slice(2)}`) : null;
    const [bin, argv] = measure ? ["/usr/bin/time", ["-f", "%e %M", "-o", timeFile, cmd, ...args]] : [cmd, args];
    const started = performance.now();
    let child;
    try {
      child = spawn(bin, argv, { cwd, env: { ...process.env, ...env }, stdio: ["ignore", "pipe", "pipe"], detached: true });
    } catch (error) {
      resolve({ code: -1, signal: null, timedOut: false, stdout: "", stderr: String(error), seconds: 0, maxRssKB: null });
      return;
    }
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    child.stdout.on("data", (chunk) => {
      if (stdout.length < CAP) stdout += chunk;
    });
    child.stderr.on("data", (chunk) => {
      stderr = (stderr + chunk).slice(-CAP);
    });
    const timer = setTimeout(() => {
      timedOut = true;
      try {
        process.kill(-child.pid, "SIGKILL");
      } catch {}
    }, timeoutMs);
    child.on("error", (error) => {
      stderr += String(error);
    });
    child.on("close", (code, signal) => {
      clearTimeout(timer);
      let seconds = (performance.now() - started) / 1000;
      let maxRssKB = null;
      if (timeFile) {
        try {
          const [elapsed, rss] = fs.readFileSync(timeFile, "utf8").trim().split(/\s+/).slice(-2).map(Number);
          if (Number.isFinite(elapsed)) seconds = elapsed;
          if (Number.isFinite(rss)) maxRssKB = rss;
        } catch {}
        fs.rmSync(timeFile, { force: true });
      }
      resolve({ code, signal, timedOut, stdout, stderr, seconds, maxRssKB });
    });
  });
}

/** Like run(), but throws with the program's error output when it fails. */
export async function must(cmd, args, options) {
  const result = await run(cmd, args, options);
  if (result.code !== 0) throw new Error(`${cmd} ${args.join(" ")} failed (${result.timedOut ? "timeout" : result.code}): ${result.stderr.slice(-2000)}`);
  return result;
}

/** Calls fn on every item, |limit| at a time, in order of the items. */
export async function pool(items, limit, fn) {
  const results = new Array(items.length);
  let next = 0;
  async function worker(slot) {
    while (next < items.length) {
      const index = next++;
      results[index] = await fn(items[index], index, slot);
    }
  }
  await Promise.all(Array.from({ length: Math.min(limit, items.length) }, (_, slot) => worker(slot)));
  return results;
}

export function sha256File(file) {
  return createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

export function readJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch {
    return fallback;
  }
}

/** Writes JSON through a temporary file, so a reader never sees half a file. */
export function writeJson(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(value, null, 2));
  fs.renameSync(tmp, file);
}

/** A file name safe on every system, from any text. */
export function slug(text) {
  return text
    .normalize("NFKD")
    .replace(/[^\w.-]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "")
    .toLowerCase()
    .slice(0, 120);
}

export function log(...parts) {
  console.log(new Date().toISOString().slice(11, 19), ...parts);
}

export const workers = Math.max(1, Number(process.env.BENCH_WORKERS) || Math.floor(os.cpus().length / 2));
