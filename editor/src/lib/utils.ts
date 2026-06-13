import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";

/// Merge conditional class lists and dedupe conflicting Tailwind utilities.
export function cn(...inputs: ClassValue[]): string {
  return twMerge(clsx(inputs));
}

/// Angle conversion factors for the UI<->wire boundary (degrees shown, radians stored).
export const RAD_TO_DEG = 180 / Math.PI;
export const DEG_TO_RAD = Math.PI / 180;
