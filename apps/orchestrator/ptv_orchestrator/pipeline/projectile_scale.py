"""Projectiles sized to the weapon that fires them.

A generated rocket is whatever size the image-to-3D model felt like, and the
manifest's ``target_scale_meters`` only fixes its LONGEST axis -- so a rocket
whose mesh came out stubby is drawn twice as fat as the launcher's tube. The
numbers to check it against are already measured: ``socket_generation``
records the projectile mesh's cross-section (``cross_section_unit``) and the
weapon's bore at its muzzle (``bore_diameter_unit``), both as fractions of
each mesh's longest axis.

Rules, every one reported:

* a realistic round / rocket / grenade is 0.6-1.3x the bore it leaves (an
  energy bolt 1-3x -- a glow is wider than the emitter), else its scale is
  corrected to 1.0x (1.5x for bolts);
* with no bore measured, the design's own calibre (``diameter_m``) is the
  reference;
* an arrow must be longer than the bow's draw, else it is lengthened to
  draw + 0.12 m;
* a projectile whose size the prompt states (``provenance.source ==
  explicit``) is checked and reported but never changed.
"""

from __future__ import annotations

from typing import Any, Optional

from ptv_schemas import AssetManifest, InteractionDesign

_REAL_BAND = (0.6, 1.3)
_ENERGY_BAND = (1.0, 3.0)
_ARROW_MARGIN_M = 0.12


def _bore_m(weapon_asset: str, muzzle: str, manifest: AssetManifest,
            contract: dict[str, Any]) -> Optional[float]:
    c = contract.get(weapon_asset) or {}
    sock = (c.get("sockets") or {}).get(muzzle) or {}
    unit = sock.get("bore_diameter_unit")
    asset = next((a for a in manifest.items if a.id == weapon_asset), None)
    if not unit or asset is None:
        return None
    return float(unit) * float(asset.target_scale_meters)


def projectile_scale_corrections(
    design: Optional[InteractionDesign],
    manifest: AssetManifest,
    contract: dict[str, Any],
) -> tuple[dict[str, float], list[str]]:
    """``({asset_id: corrected target_scale_meters}, notes)``."""
    if design is None:
        return {}, []
    scales: dict[str, float] = {}
    notes: list[str] = []
    assets = {a.id: a for a in manifest.items}
    for item in design.items:
        w = item.weapon
        if w is None or not w.projectile_id:
            continue
        p = design.projectile(w.projectile_id)
        if p is None or not p.asset_id or p.asset_id not in assets:
            continue
        cs_unit = (contract.get(p.asset_id) or {}).get("cross_section_unit")
        if not cs_unit:
            notes.append(f"{p.id}: no measured cross-section for '{p.asset_id}'; scale unchecked")
            continue
        scale = scales.get(p.asset_id, float(assets[p.asset_id].target_scale_meters))
        diameter = float(cs_unit) * scale
        muzzle = next((s.name for s in item.sockets if s.kind.value == "muzzle"), "muzzle")
        bore = _bore_m(item.asset_id, muzzle, manifest, contract)
        ref, what = (bore, f"{item.asset_id}'s bore") if bore else (p.diameter_m, "its calibre")
        energy = p.kind.value == "energy_bolt"
        lo, hi = _ENERGY_BAND if energy else _REAL_BAND
        ratio = diameter / ref if ref > 0 else 1.0
        msg = (f"{p.id}: drawn {diameter * 1000:.1f} mm across vs {what} "
               f"{ref * 1000:.1f} mm ({ratio:.2f}x, allowed {lo}-{hi}x)")
        if lo <= ratio <= hi:
            notes.append(msg + " -- ok")
            continue
        if p.provenance.source.value == "explicit":
            notes.append(msg + " -- the prompt sets this size, kept")
            continue
        want = (1.5 if energy else 1.0) * ref
        new_scale = round(scale * want / diameter, 4)
        scales[p.asset_id] = new_scale
        notes.append(msg + f" -- rescaled '{p.asset_id}' {scale:.3f} m -> {new_scale:.3f} m")

    for item in design.items:
        b = item.bow
        if b is None or not b.arrow_asset_id or b.arrow_asset_id not in assets:
            continue
        p = design.projectile(b.arrow_projectile_id)
        length = scales.get(b.arrow_asset_id, float(assets[b.arrow_asset_id].target_scale_meters))
        need = b.max_draw_m + _ARROW_MARGIN_M
        msg = (f"{b.arrow_asset_id}: arrow {length:.2f} m for a {b.max_draw_m:.2f} m draw "
               f"(needs >= {need:.2f} m)")
        if length >= need:
            notes.append(msg + " -- ok")
        elif p is not None and p.provenance.source.value == "explicit":
            notes.append(msg + " -- the prompt sets this size, kept")
        else:
            scales[b.arrow_asset_id] = round(need, 4)
            notes.append(msg + f" -- lengthened to {need:.2f} m")
    return scales, notes


def apply_scales(manifest: AssetManifest, scales: dict[str, float]) -> AssetManifest:
    if not scales:
        return manifest
    items = [a.model_copy(update={"target_scale_meters": scales[a.id]}) if a.id in scales
             else a for a in manifest.items]
    return manifest.model_copy(update={"items": items})
