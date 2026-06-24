// A reusable paste-and-store field for a connector's API key. The secret is written to
// the OS keyring via the bridge and never read back — only its presence is reflected.
// Shared by the Store onboarding panel and the Settings "API & Secrets" section.
import { useEffect, useState } from "react";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";

import { errorText, notify, notifyError } from "../lib/flash";
import { connectorClearSecret, connectorSecretStatus, connectorSetSecret } from "./types";

export function ApiKeyField({
  connectorId,
  displayName,
  onStatusChange,
}: {
  connectorId: string;
  displayName: string;
  onStatusChange?: (hasKey: boolean) => void;
}) {
  const [hasKey, setHasKey] = useState(false);
  const [value, setValue] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    connectorSecretStatus(connectorId)
      .then((has) => {
        setHasKey(has);
        onStatusChange?.(has);
      })
      .catch(() => {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connectorId]);

  const save = () => {
    const trimmed = value.trim();
    if (!trimmed) return;
    setBusy(true);
    connectorSetSecret(connectorId, trimmed)
      .then(() => {
        setHasKey(true);
        setValue("");
        onStatusChange?.(true);
        notify(`${displayName} key saved on this machine`);
      })
      .catch((err: unknown) => notifyError(errorText(err)))
      .finally(() => setBusy(false));
  };

  const clear = () => {
    setBusy(true);
    connectorClearSecret(connectorId)
      .then(() => {
        setHasKey(false);
        onStatusChange?.(false);
        notify(`${displayName} key cleared`);
      })
      .catch((err: unknown) => notifyError(errorText(err)))
      .finally(() => setBusy(false));
  };

  return (
    <div className="flex items-center gap-2">
      <Input
        type="password"
        placeholder={hasKey ? "•••••••• (key set)" : `${displayName} API key`}
        value={value}
        onChange={(e) => setValue(e.target.value)}
        disabled={busy}
        className="h-7 flex-1 text-xs"
      />
      <Button
        type="button"
        size="sm"
        className="h-7 text-xs"
        disabled={busy || !value.trim()}
        onClick={save}
      >
        Save
      </Button>
      {hasKey ? (
        <Button
          type="button"
          size="sm"
          variant="ghost"
          className="h-7 text-xs"
          disabled={busy}
          onClick={clear}
        >
          Clear
        </Button>
      ) : null}
    </div>
  );
}
