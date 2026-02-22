// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import sharp from "sharp";

const MAX_IMAGE_DIMENSION = 1568;
const MAX_IMAGE_PIXELS = 1.15 * 1024 * 1024; // ~1.15 megapixels
const MAX_TEXT_LENGTH = 4000;

export async function transformMcpResponse(
  response: unknown,
): Promise<unknown> {
  if (!isJsonRpcResponse(response)) return response;

  const result = (response as any).result;
  if (!result?.content || !Array.isArray(result.content)) return response;

  const resp = response as Record<string, any>;
  const transformed = { ...resp, result: { ...result, content: [] as any[] } };

  for (const item of result.content) {
    if (item.type === "image" && item.data) {
      transformed.result.content.push(await scaleImage(item));
    } else if (item.type === "text" && typeof item.text === "string") {
      transformed.result.content.push(truncateText(item));
    } else {
      transformed.result.content.push(item);
    }
  }

  return transformed;
}

async function scaleImage(item: any): Promise<any> {
  try {
    const buf = Buffer.from(item.data, "base64");
    const img = sharp(buf);
    const meta = await img.metadata();
    if (!meta.width || !meta.height) return item;

    const pixels = meta.width * meta.height;
    const shrink = Math.min(
      MAX_IMAGE_DIMENSION / meta.width,
      MAX_IMAGE_DIMENSION / meta.height,
      Math.sqrt(MAX_IMAGE_PIXELS / pixels),
      1, // never upscale
    );

    if (shrink >= 1) {
      // No scaling needed, but ensure webp format
      if (item.mimeType === "image/webp") return item;
      const webp = await img.webp({ quality: 80 }).toBuffer();
      return { ...item, data: webp.toString("base64"), mimeType: "image/webp" };
    }

    const width = Math.round(meta.width * shrink);
    const height = Math.round(meta.height * shrink);
    // Always re-encode as webp for smallest size
    const scaled = await img
      .resize(width, height)
      .webp({ quality: 80 })
      .toBuffer();

    return { ...item, data: scaled.toString("base64"), mimeType: "image/webp" };
  } catch {
    return item; // if scaling fails, pass through original
  }
}

function truncateText(item: any): any {
  if (item.text.length <= MAX_TEXT_LENGTH) return item;
  return {
    ...item,
    text: item.text.slice(0, MAX_TEXT_LENGTH) + "\n... [truncated]",
  };
}

function isJsonRpcResponse(msg: unknown): boolean {
  return (
    typeof msg === "object" &&
    msg !== null &&
    "jsonrpc" in msg &&
    "id" in msg &&
    "result" in msg
  );
}
