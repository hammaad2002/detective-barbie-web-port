/**
 * Detective Barbie runs at 640×480 with no separate graphics settings file in the install tree.
 * This module keeps the settings panel API (resolution / Esc alias) without D3D-style options.
 */
import { KEYS } from "./keys.ts";

export const GAME_SUBDIR = "";

export const AUTO_RESOLUTION = "auto";

export const RESOLUTIONS: { group: string; sizes: [number, number][] }[] = [
  { group: "4:3", sizes: [[640, 480], [800, 600], [1024, 768], [1280, 960]] },
];

/** Native game resolution. */
export function matchScreen(): [number, number] {
  return [640, 480];
}

export type Adv = Record<string, number>;

export const ADV_OPTIONS: { key: string; label: string; choices?: string[] }[] = [];

export const PRESETS: Record<string, Adv> = { Default: {} };
export const DEFAULT_QUALITY = "Default";

export function presetOf(_adv: Adv): string | undefined {
  return "Default";
}

export interface Graphics {
  width: number;
  height: number;
  adv: Adv;
}

export async function loadGraphics(): Promise<Graphics> {
  return { width: 640, height: 480, adv: {} };
}

/** No settings.dat for this title; kept for API compatibility with the settings panel. */
export async function applyGraphics(_resolution?: string, _adv?: Adv): Promise<void> {}

export function keyLabel(code: string): string {
  const named: Record<string, string> = {
    ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→", Escape: "Esc", Space: "Space", Enter: "Enter",
    NumpadEnter: "Num Enter", ShiftLeft: "Left Shift", ShiftRight: "Right Shift", ControlLeft: "Left Ctrl",
    ControlRight: "Right Ctrl", AltLeft: "Left Alt", AltRight: "Right Alt", Backquote: "`", Minus: "-", Equal: "=",
    BracketLeft: "[", BracketRight: "]", Backslash: "\\", Semicolon: ";", Quote: "'", Comma: ",", Period: ".", Slash: "/",
    NumpadMultiply: "Num *", NumpadSubtract: "Num -", NumpadAdd: "Num +", NumpadDecimal: "Num .", NumpadDivide: "Num /",
    PageUp: "Page Up", PageDown: "Page Down", CapsLock: "Caps Lock", NumLock: "Num Lock", ScrollLock: "Scroll Lock",
  };
  if (named[code]) return named[code];
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit\d$/.test(code)) return code.slice(5);
  if (/^Numpad\d$/.test(code)) return `Num ${code.slice(6)}`;
  return code;
}

export const isGameKey = (code: string) => code in KEYS;
