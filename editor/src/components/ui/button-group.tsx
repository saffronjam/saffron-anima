/// shadcn ButtonGroup — a CSS-only wrapper that lays its children in a row (or column) and
/// collapses the rounded corners + doubled borders between adjacent items, so a Select + Button
/// (or several buttons) read as one segmented control. Composes with the local Button + Select.
import type * as React from "react";
import { cva, type VariantProps } from "class-variance-authority";
import { cn } from "@/lib/utils";

const buttonGroupVariants = cva("flex w-fit items-stretch [&>*]:relative [&>*:focus-within]:z-10", {
  variants: {
    orientation: {
      horizontal:
        "[&>*:not(:first-child)]:rounded-l-none [&>*:not(:first-child)]:border-l-0 [&>*:not(:last-child)]:rounded-r-none",
      vertical:
        "flex-col [&>*:not(:first-child)]:rounded-t-none [&>*:not(:first-child)]:border-t-0 [&>*:not(:last-child)]:rounded-b-none",
    },
  },
  defaultVariants: { orientation: "horizontal" },
});

export function ButtonGroup({
  className,
  orientation,
  ...props
}: React.ComponentProps<"div"> & VariantProps<typeof buttonGroupVariants>) {
  return (
    <div
      data-slot="button-group"
      data-orientation={orientation ?? "horizontal"}
      role="group"
      className={cn(buttonGroupVariants({ orientation }), className)}
      {...props}
    />
  );
}
