// ImageRow.tsx — a static image block (`type:"image"`).
//
// Ports `buildImage` (settings/main.legacy.js:634-648).
//
// The `src` is UNTRUSTED author text going into an <img>, so it is confined to
// the mod's own `views/<modId>/` folder by @lib/settings/assets before it gets
// here. A REJECTED path is not silently dropped — it renders a warn note naming
// the rule, because a schema author staring at a blank space has no way to
// discover that their "../shared/banner.png" was refused.
//
// The caller resolves the path (and, in the harness only, supplies the asset
// roots) — this component never resolves anything itself, which is what keeps
// `window.OSFUI_MOD_ASSET_ROOTS` out of src/ entirely.

export interface ImageRowProps {
  /** Already through safeAssetSrc. Null means REJECTED — render the warning. */
  src: string | null;
  caption: string;
  /** Schema `height` in px, or 0/undefined for none. */
  height: number | undefined;
  /** tr("imageRejected", …) — passed in so this file stays localiser-free. */
  rejectedText: string;
}

export function ImageRow({ src, caption, height, rejectedText }: ImageRowProps) {
  return (
    <figure class="osf-figure">
      {src ? (
        <img
          class="osf-image"
          src={src}
          // The caption doubles as alt text; "" when there is none, which is
          // the correct encoding for a decorative image.
          alt={caption}
          // `| 0` truncates toward zero exactly as legacy did (main.legacy.js:640),
          // so a fractional or absurd height still yields an integer px value.
          // Applied only when truthy — `height: 0` means "no cap", not "0px".
          {...(height ? { style: { maxHeight: `${height | 0}px` } } : {})}
        />
      ) : (
        <div class="osf-note osf-note--warn">{rejectedText}</div>
      )}
      {caption ? <figcaption class="osf-figcaption">{caption}</figcaption> : null}
    </figure>
  );
}
