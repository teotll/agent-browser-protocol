// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// === Common Envelope Types ===

export interface WaitUntil {
  type?: "immediate" | "action_complete" | "time";
  timeout_ms?: number;
  duration_ms?: number;
}

export interface ScreenshotOptions {
  area?: "none" | "viewport";
  disable_markup?: ("clickable" | "typeable" | "scrollable" | "grid" | "selected")[];
  cursor?: boolean;
  format?: string;
}

export interface ActionRequest {
  wait_until?: WaitUntil;
  screenshot?: ScreenshotOptions;
}

export interface ScreenshotData {
  data: string;
  width: number;
  height: number;
  virtual_time_ms: number;
  format: string;
}

export interface ScrollPosition {
  horizontal_percent: number;
  vertical_percent: number;
  horizontal_px: number;
  vertical_px: number;
  page_width: number;
  page_height: number;
  viewport_width: number;
  viewport_height: number;
}

export interface ActionTiming {
  action_started_ms: number;
  action_completed_ms: number;
  wait_completed_ms: number;
  duration_ms: number;
}

export interface ActionEvent {
  type: string;
  virtual_time_ms: number;
  data: Record<string, unknown>;
}

export interface ActionResponse<T = Record<string, unknown>> {
  result: T;
  screenshot_before?: ScreenshotData;
  screenshot_after?: ScreenshotData;
  scroll?: ScrollPosition;
  events?: ActionEvent[];
  timing?: ActionTiming;
}

export interface ErrorResponse {
  error: string;
}

// === Browser ===

export interface BrowserStatus {
  success: boolean;
  data: {
    ready: boolean;
    state: string;
    components: {
      http_server: boolean;
      browser_window: boolean;
      devtools: boolean;
    };
    message?: string;
  };
}

export interface SessionData {
  success: boolean;
  data: {
    session_dir: string;
    database_path: string;
    screenshots_dir: string;
    screenshots_enabled: boolean;
  };
}

// === Tabs ===

export interface Tab {
  id: string;
  url: string;
  title: string;
  active?: boolean;
  loading?: boolean;
}

export interface CreateTabOptions {
  url?: string;
  active?: boolean;
  index?: number;
}

export interface CreatedTab {
  id: string;
  url: string;
}

export interface ActivateResult {
  status: string;
  tab_id: string;
  index: number;
}

// === Navigation ===

export type Modifier = "Shift" | "Control" | "Alt" | "Meta"
  | "ShiftLeft" | "ShiftRight" | "ControlLeft" | "ControlRight"
  | "AltLeft" | "AltRight" | "MetaLeft" | "MetaRight";

export interface NavigateOptions extends ActionRequest {
  url: string;
  referrer?: string;
}

// === Mouse ===

export interface ClickOptions extends ActionRequest {
  x: number;
  y: number;
  button?: "left" | "right" | "middle";
  click_count?: number;
  modifiers?: Modifier[];
}

export interface MoveOptions {
  x: number;
  y: number;
}

export interface ScrollOptions extends ActionRequest {
  x: number;
  y: number;
  delta_x?: number;
  delta_y?: number;
}

export interface DragOptions extends ActionRequest {
  start_x: number;
  start_y: number;
  end_x: number;
  end_y: number;
  steps?: number;
}

// === Keyboard ===

export interface TypeOptions extends ActionRequest {
  text: string;
}

export interface KeyOptions extends ActionRequest {
  key: string;
  modifiers?: Modifier[];
}

// === Content ===

export interface ExecuteOptions extends ActionRequest {
  script: string;
}

export interface ExecuteResult {
  value: unknown;
  type: string;
}

export interface TextOptions extends ActionRequest {
  selector?: string;
}

export interface TextResult {
  text: string | null;
}

// === Wait ===

export interface WaitOptions extends ActionRequest {
  ms: number;
}

// === Dialogs ===

export interface DialogInfo {
  present: boolean;
  dialog_type?: string;
  message?: string;
  default_prompt?: string;
}

export interface AcceptDialogOptions {
  prompt_text?: string;
}

// === Execution Control ===

export interface ExecutionState {
  enabled: boolean;
  paused: boolean;
  virtual_time_base_ms?: number;
}

export interface SetExecutionOptions {
  paused: boolean;
  initial_virtual_time?: number;
}

// === Downloads ===

export interface Download {
  id: string;
  url: string;
  filename: string;
  path?: string;
  state: string;
  bytes_received: number;
  total_bytes: number;
  percent_complete?: number;
  mime_type: string;
  start_time: number;
  end_time?: number;
}

export interface ListDownloadsOptions {
  state?: string;
  limit?: number;
}

// === File Chooser ===

export interface FileChooserOpenOptions {
  files: string[];
}

export interface FileChooserSaveOptions {
  path: string;
}

export interface FileChooserCancelOptions {
  cancel: true;
}

export type FileChooserOptions =
  | FileChooserOpenOptions
  | FileChooserSaveOptions
  | FileChooserCancelOptions;

// === History ===

export interface Session {
  id: string;
  [key: string]: unknown;
}

export interface HistoryAction {
  id: string;
  [key: string]: unknown;
}

export interface HistoryEvent {
  id: string;
  [key: string]: unknown;
}

// === Shutdown ===

export interface ShutdownOptions {
  timeout_ms?: number;
}
