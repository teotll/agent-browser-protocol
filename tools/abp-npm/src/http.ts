// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import http from "node:http";
import https from "node:https";
import { URL } from "node:url";

export interface RequestOptions {
  method?: string;
  headers?: Record<string, string>;
  body?: unknown;
  timeout?: number;
}

export interface Response<T = unknown> {
  status: number;
  headers: Record<string, string | string[] | undefined>;
  data: T;
}

export async function request<T = unknown>(
  url: string,
  options: RequestOptions = {},
): Promise<Response<T>> {
  const { method = "GET", headers = {}, body, timeout = 30000 } = options;
  const parsed = new URL(url);
  const transport = parsed.protocol === "https:" ? https : http;

  return new Promise((resolve, reject) => {
    const req = transport.request(
      parsed,
      {
        method,
        headers: {
          ...(body !== undefined
            ? { "Content-Type": "application/json" }
            : {}),
          ...headers,
        },
        timeout,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk: Buffer) => chunks.push(chunk));
        res.on("end", () => {
          const raw = Buffer.concat(chunks);
          const contentType = res.headers["content-type"] || "";
          let data: unknown;
          if (contentType.includes("application/json")) {
            data = JSON.parse(raw.toString("utf-8"));
          } else if (
            contentType.includes("image/") ||
            contentType.includes("application/octet-stream")
          ) {
            data = raw;
          } else {
            data = raw.toString("utf-8");
          }
          resolve({
            status: res.statusCode || 0,
            headers: res.headers as Record<
              string,
              string | string[] | undefined
            >,
            data: data as T,
          });
        });
      },
    );
    req.on("error", reject);
    req.on("timeout", () => {
      req.destroy();
      reject(new Error(`Request to ${url} timed out after ${timeout}ms`));
    });
    if (body !== undefined) {
      req.write(JSON.stringify(body));
    }
    req.end();
  });
}

export async function downloadToFile(
  url: string,
  destPath: string,
): Promise<void> {
  const fs = await import("node:fs");
  const parsed = new URL(url);
  const transport = parsed.protocol === "https:" ? https : http;

  return new Promise((resolve, reject) => {
    transport.get(parsed, { timeout: 300000 }, (res) => {
      if (
        (res.statusCode === 301 || res.statusCode === 302) &&
        res.headers.location
      ) {
        res.resume();
        downloadToFile(res.headers.location, destPath).then(resolve, reject);
        return;
      }

      if (res.statusCode !== 200) {
        res.resume();
        reject(
          new Error(
            `Download failed: HTTP ${res.statusCode} from ${url}`,
          ),
        );
        return;
      }

      const file = fs.createWriteStream(destPath);
      res.pipe(file);
      file.on("finish", () => {
        file.close();
        resolve();
      });
      file.on("error", (err) => {
        fs.unlinkSync(destPath);
        reject(err);
      });
    }).on("error", reject);
  });
}
