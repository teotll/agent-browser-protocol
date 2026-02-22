// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

export { ABPClient } from "./client.js";
export { launch } from "./launch.js";
export { getExecutablePath } from "./paths.js";
export { ABP_VERSION, CHROME_VERSION } from "./paths.js";
export type { LaunchOptions, Browser } from "./launch.js";
export type {
  // Common
  WaitUntil,
  ScreenshotOptions,
  ActionRequest,
  ActionResponse,
  ScreenshotData,
  ScrollPosition,
  ActionTiming,
  ActionEvent,
  ErrorResponse,
  // Browser
  BrowserStatus,
  SessionData,
  ShutdownOptions,
  // Tabs
  Tab,
  CreateTabOptions,
  CreatedTab,
  ActivateResult,
  // Navigation
  Modifier,
  NavigateOptions,
  // Mouse
  ClickOptions,
  MoveOptions,
  ScrollOptions,
  // Keyboard
  TypeOptions,
  KeyOptions,
  // Content
  ExecuteOptions,
  ExecuteResult,
  TextOptions,
  TextResult,
  // Wait
  WaitOptions,
  // Dialogs
  DialogInfo,
  AcceptDialogOptions,
  // Execution Control
  ExecutionState,
  SetExecutionOptions,
  // Downloads
  Download,
  ListDownloadsOptions,
  // File Chooser
  FileChooserOptions,
  FileChooserOpenOptions,
  FileChooserSaveOptions,
  FileChooserCancelOptions,
  // History
  Session,
  HistoryAction,
  HistoryEvent,
} from "./types.js";
