
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
          alt={caption}
          {...(height ? { style: { maxHeight: `${height | 0}px` } } : {})}
        />
      ) : (
        <div class="osf-note osf-note--warn">{rejectedText}</div>
      )}
      {caption ? <figcaption class="osf-figcaption">{caption}</figcaption> : null}
    </figure>
  );
}
