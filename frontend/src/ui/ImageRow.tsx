// Static image block (`type:"image"`).
//
// `src` is untrusted author text going into an <img>, so @lib/settings/assets
// confines it to the mod's own `views/<modId>/` folder before it gets here. A
// rejected path renders a warn note naming the rule rather than nothing, so an
// author can tell why "../shared/banner.png" produced a blank space.
//
// The caller resolves the path (and, in the harness only, supplies the asset
// roots); this component resolves nothing, which keeps
// `window.OSFUI_MOD_ASSET_ROOTS` out of src/.

export interface ImageRowProps {
  /** Already through safeAssetSrc. Null means rejected — render the warning. */
  src: string | null;
  caption: string;
  /** Schema `height` in px, or 0/undefined for none. */
  height: number | undefined;
  /** tr("imageRejected", …) — passed in so this file stays localiser-free. */
  rejectedText: string;
  /** `visibleWhen` said no. */
  hiddenCond: boolean;
}

export function ImageRow({ src, caption, height, rejectedText, hiddenCond }: ImageRowProps) {
  return (
    <figure class={hiddenCond ? 'osf-figure hidden-cond' : 'osf-figure'}>
      {src ? (
        <img
          class="osf-image"
          src={src}
          // Caption doubles as alt text; "" when there is none, the correct
          // encoding for a decorative image.
          alt={caption}
          // `| 0` truncates toward zero, so a fractional or absurd height still
          // yields an integer px value. Applied only when truthy — `height: 0`
          // means "no cap", not "0px".
          {...(height ? { style: { maxHeight: `${height | 0}px` } } : {})}
        />
      ) : (
        <div class="osf-note osf-note--warn">{rejectedText}</div>
      )}
      {caption ? <figcaption class="osf-figcaption">{caption}</figcaption> : null}
    </figure>
  );
}
