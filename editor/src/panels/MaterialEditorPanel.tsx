/// The Material editor: pick a .smat material asset, see it on a studio-lit preview sphere, and
/// edit its factors live. Reads/writes over the control plane (material-list/get/update/create +
/// preview-render); edits are coalesced and re-render the preview. Texture-slot picking is the
/// entity inspector's job (assign-asset); this panel edits the shared material asset's factors.
import { useCallback, useEffect, useRef, useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { renderField, type FieldRenderContext } from "../components/fieldRenderer";
import { makeCoalescer, type Coalescer } from "../control/coalesce";
import { errorText, notifyError } from "../lib/flash";
import { MaterialGraphEditor } from "./MaterialGraphEditor";
import { Button } from "@/components/ui/button";
import { Label } from "@/components/ui/label";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

const FACTOR_FIELDS = ["baseColor", "metallic", "roughness", "emissive", "emissiveStrength"] as const;
const TEXTURE_FIELDS = [
  "albedoTexture",
  "ormTexture",
  "normalTexture",
  "emissiveTexture",
  "heightTexture",
] as const;

interface MaterialRef {
  id: string;
  name: string;
}

export function MaterialEditorPanel() {
  const selectedMaterialId = useEditorStore((s) => s.selectedMaterialId);
  const setSelectedMaterialId = useEditorStore((s) => s.setSelectedMaterialId);
  const setDragActive = useEditorStore((s) => s.setDragActive);

  const [materials, setMaterials] = useState<MaterialRef[]>([]);
  const [fields, setFields] = useState<Record<string, unknown> | null>(null);
  const [preview, setPreview] = useState<string | null>(null);
  const [graphOpen, setGraphOpen] = useState(false);
  const coalescers = useRef<Map<string, Coalescer<unknown>>>(new Map());

  const refreshList = useCallback(async () => {
    try {
      const result = await client.materialList();
      setMaterials(result.materials.map((m) => ({ id: m.id, name: m.name })));
    } catch (err) {
      notifyError(errorText(err));
    }
  }, []);

  useEffect(() => {
    void refreshList();
  }, [refreshList]);

  const refreshPreview = useCallback(async (id: string) => {
    try {
      const result = await client.previewRender(id, 256);
      setPreview(result.png);
    } catch (err) {
      notifyError(errorText(err));
    }
  }, []);

  useEffect(() => {
    coalescers.current.clear();
    if (!selectedMaterialId) {
      setFields(null);
      setPreview(null);
      return;
    }
    const id = selectedMaterialId;
    void (async () => {
      try {
        const material = await client.materialGet(id);
        setFields(material as unknown as Record<string, unknown>);
      } catch (err) {
        notifyError(errorText(err));
      }
    })();
    void refreshPreview(id);
  }, [selectedMaterialId, refreshPreview]);

  const editField = useCallback(
    (field: string, value: unknown) => {
      if (!selectedMaterialId) {
        return;
      }
      const id = selectedMaterialId;
      setFields((current) => (current ? { ...current, [field]: value } : current));
      let coalescer = coalescers.current.get(field);
      if (!coalescer) {
        coalescer = makeCoalescer<unknown>({
          send: async (latest) => {
            const patch = { [field]: latest } as Parameters<typeof client.materialUpdate>[1];
            await client.materialUpdate(id, patch);
            await refreshPreview(id);
          },
        });
        coalescers.current.set(field, coalescer);
      }
      coalescer.push(value);
    },
    [selectedMaterialId, refreshPreview],
  );

  const newMaterial = useCallback(async () => {
    try {
      const created = await client.materialCreate("Material");
      await refreshList();
      setSelectedMaterialId(created.id);
    } catch (err) {
      notifyError(errorText(err));
    }
  }, [refreshList, setSelectedMaterialId]);

  const ctx: FieldRenderContext = {
    onDragStart: () => setDragActive(true),
    onDragEnd: () => setDragActive(false),
  };

  return (
    <div className="flex h-full flex-col gap-3 overflow-y-auto bg-neutral-900 p-3 text-[12px] text-neutral-200">
      <div className="flex items-center gap-2">
        <Select
          value={selectedMaterialId ?? ""}
          onValueChange={(value) => setSelectedMaterialId(value || null)}
        >
          <SelectTrigger size="sm" className="h-7 w-full text-[11px]">
            <SelectValue placeholder="Select a material…" />
          </SelectTrigger>
          <SelectContent>
            {materials.map((m) => (
              <SelectItem key={m.id} value={m.id} className="text-[11px]">
                {m.name}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
        <Button size="sm" onClick={() => void newMaterial()}>
          New
        </Button>
        <Button
          size="sm"
          variant="secondary"
          disabled={!selectedMaterialId}
          onClick={() => setGraphOpen(true)}
        >
          Graph
        </Button>
      </div>

      {preview ? (
        <img
          src={`data:image/png;base64,${preview}`}
          alt="material preview"
          className="aspect-square w-full rounded border border-neutral-700 object-cover"
        />
      ) : (
        <div className="flex aspect-square w-full items-center justify-center rounded border border-dashed border-neutral-700 text-neutral-500">
          {selectedMaterialId ? "Rendering…" : "No material selected"}
        </div>
      )}

      {fields
        ? FACTOR_FIELDS.map((field) => (
            <div key={field} className="flex flex-col gap-1">
              <Label className="text-[11px] capitalize text-neutral-400">{field}</Label>
              {renderField("Material", field, fields[field], (next) => editField(field, next), ctx)}
            </div>
          ))
        : null}

      {fields
        ? TEXTURE_FIELDS.map((field) => (
            <div key={field} className="flex flex-col gap-1">
              <Label className="text-[11px] text-neutral-400">{field}</Label>
              {renderField("Material", field, fields[field], (next) => editField(field, next), ctx)}
            </div>
          ))
        : null}

      {graphOpen && selectedMaterialId ? (
        <MaterialGraphEditor
          materialId={selectedMaterialId}
          onClose={() => {
            setGraphOpen(false);
            void refreshPreview(selectedMaterialId);
          }}
        />
      ) : null}
    </div>
  );
}
