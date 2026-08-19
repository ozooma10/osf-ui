
import { createPortal } from 'preact/compat';
import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import type { JSX } from 'preact';
import { cx } from './cx';

const VIEWPORT_MARGIN = 8;
const MENU_GAP = 4;
const OPTION_HEIGHT = 32;
const MAX_MENU_HEIGHT = 320;

export interface DropdownOption {
  value: string;
  label: string;
  disabled?: boolean;
}

export interface DropdownAnchorRect {
  left: number;
  right: number;
  top: number;
  bottom: number;
  width: number;
}

export interface DropdownPlacement {
  left: number;
  top: number;
  width: number;
  maxHeight: number;
  opensUp: boolean;
}

export function dropdownPlacement(
  anchor: DropdownAnchorRect,
  viewportWidth: number,
  viewportHeight: number,
  desiredHeight: number,
  desiredWidth = anchor.width,
): DropdownPlacement {
  const safeWidth = Math.max(1, viewportWidth - VIEWPORT_MARGIN * 2);
  const width = Math.max(1, Math.min(Math.max(anchor.width, desiredWidth), safeWidth));
  const maxLeft = Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width);
  const left = Math.min(Math.max(VIEWPORT_MARGIN, anchor.left), maxLeft);
  const below = Math.max(0, viewportHeight - VIEWPORT_MARGIN - anchor.bottom - MENU_GAP);
  const above = Math.max(0, anchor.top - VIEWPORT_MARGIN - MENU_GAP);
  const opensUp = below < desiredHeight && above > below;
  const available = opensUp ? above : below;
  const maxHeight = Math.max(0, Math.min(desiredHeight, available));
  const top = opensUp
    ? Math.max(VIEWPORT_MARGIN, anchor.top - MENU_GAP - maxHeight)
    : Math.min(viewportHeight - VIEWPORT_MARGIN, anchor.bottom + MENU_GAP);

  return { left, top, width, maxHeight, opensUp };
}

export interface DropdownProps {
  id: string;
  value: string | undefined;
  options: readonly DropdownOption[];
  disabled: boolean;
  onCommit: (value: string) => void;
  /** Extra class on the in-flow wrapper. */
  class?: string;
  /** Extra class on the body-level menu portal. */
  menuClass?: string;
  /** Optional direct label; omit when a label[for=id] names the trigger. */
  ariaLabel?: string;
}

function selectedIndex(options: readonly DropdownOption[], value: string | undefined): number {
  const found = options.findIndex((option) => option.value === value);
  return found >= 0 ? found : options.length ? 0 : -1;
}

function firstEnabled(options: readonly DropdownOption[], from: number, delta: 1 | -1): number {
  for (let index = from; index >= 0 && index < options.length; index += delta) {
    if (!options[index]!.disabled) return index;
  }
  return -1;
}

function keyName(event: JSX.TargetedKeyboardEvent<HTMLButtonElement>): string {
  if (event.key) return event.key;
  switch (event.keyCode) {
    case 9: return 'Tab';
    case 13: return 'Enter';
    case 27: return 'Escape';
    case 32: return ' ';
    case 35: return 'End';
    case 36: return 'Home';
    case 38: return 'ArrowUp';
    case 40: return 'ArrowDown';
    default: return '';
  }
}

export function Dropdown(props: DropdownProps) {
  const triggerRef = useRef<HTMLButtonElement | null>(null);
  const menuRef = useRef<HTMLDivElement | null>(null);
  const typeahead = useRef({ text: '', at: 0 });
  const [open, setOpen] = useState(false);
  const [activeIndex, setActiveIndex] = useState(() => selectedIndex(props.options, props.value));
  const [placement, setPlacement] = useState<DropdownPlacement | null>(null);

  const listId = `${props.id}-listbox`;
  const optionId = (index: number) => `${props.id}-option-${index}`;
  const selected = selectedIndex(props.options, props.value);
  const current = selected >= 0 ? props.options[selected] : undefined;
  const inert = props.disabled || props.options.length === 0;

  const measure = (desiredWidth?: number) => {
    const trigger = triggerRef.current;
    if (!trigger) return;
    const rect = trigger.getBoundingClientRect();
    const desiredHeight = Math.min(
      MAX_MENU_HEIGHT,
      props.options.length * OPTION_HEIGHT + 2,
    );
    setPlacement(dropdownPlacement(
      rect,
      window.innerWidth,
      window.innerHeight,
      desiredHeight,
      desiredWidth,
    ));
  };

  const close = (restoreFocus = false) => {
    setOpen(false);
    typeahead.current = { text: '', at: 0 };
    if (restoreFocus) triggerRef.current?.focus();
  };

  const openAt = (where: 'selected' | 'first' | 'last') => {
    if (inert) return;
    let next = selected;
    if (where === 'first') next = firstEnabled(props.options, 0, 1);
    if (where === 'last') next = firstEnabled(props.options, props.options.length - 1, -1);
    if (next < 0 || props.options[next]!.disabled) {
      next = firstEnabled(props.options, 0, 1);
    }
    setActiveIndex(next);
    measure();
    setOpen(true);
  };

  const choose = (index: number) => {
    const option = props.options[index];
    if (!option || option.disabled) return;
    props.onCommit(option.value);
    close(true);
  };

  const move = (delta: 1 | -1) => {
    const start = activeIndex < 0
      ? (delta > 0 ? 0 : props.options.length - 1)
      : activeIndex + delta;
    const next = firstEnabled(props.options, start, delta);
    if (next >= 0) setActiveIndex(next);
  };

  const handleTypeahead = (key: string) => {
    const now = Date.now();
    const previous = now - typeahead.current.at <= 700 ? typeahead.current.text : '';
    const lower = key.toLocaleLowerCase();
    const text = previous === lower ? lower : previous + lower;
    typeahead.current = { text, at: now };

    const start = activeIndex >= 0 ? activeIndex + 1 : 0;
    for (let offset = 0; offset < props.options.length; offset += 1) {
      const index = (start + offset) % props.options.length;
      const option = props.options[index]!;
      if (!option.disabled && option.label.toLocaleLowerCase().startsWith(text)) {
        setActiveIndex(index);
        return;
      }
    }
  };

  const onKeyDown = (event: JSX.TargetedKeyboardEvent<HTMLButtonElement>) => {
    const key = keyName(event);
    if (!open) {
      if (key === 'ArrowDown' || key === 'Home') {
        event.preventDefault();
        event.stopPropagation();
        openAt('first');
      } else if (key === 'ArrowUp' || key === 'End') {
        event.preventDefault();
        event.stopPropagation();
        openAt('last');
      }
      return;
    }

    if (key === 'Tab') {
      close();
      return;
    }
    if (key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      close(true);
      return;
    }
    if (key === 'ArrowDown' || key === 'ArrowUp') {
      event.preventDefault();
      event.stopPropagation();
      move(key === 'ArrowDown' ? 1 : -1);
      return;
    }
    if (key === 'Home' || key === 'End') {
      event.preventDefault();
      event.stopPropagation();
      setActiveIndex(firstEnabled(
        props.options,
        key === 'Home' ? 0 : props.options.length - 1,
        key === 'Home' ? 1 : -1,
      ));
      return;
    }
    if (key === 'Enter' || key === ' ') {
      event.preventDefault();
      event.stopPropagation();
      if (activeIndex >= 0) choose(activeIndex);
      return;
    }
    if (key.length === 1 && !event.ctrlKey && !event.altKey && !event.metaKey) {
      event.preventDefault();
      event.stopPropagation();
      handleTypeahead(key);
    }
  };

  useEffect(() => {
    if (!open) return;
    const next = selectedIndex(props.options, props.value);
    setActiveIndex(next >= 0 && !props.options[next]!.disabled
      ? next
      : firstEnabled(props.options, 0, 1));
    measure();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, props.options, props.value]);

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: PointerEvent) => {
      const target = event.target as Node | null;
      if (!target || triggerRef.current?.contains(target) || menuRef.current?.contains(target)) return;
      close();
    };
    const onEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' && event.keyCode !== 27) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      close(true);
    };
    const reposition = () => measure();
    document.addEventListener('pointerdown', onPointerDown, true);
    document.addEventListener('keydown', onEscape, true);
    document.addEventListener('scroll', reposition, true);
    window.addEventListener('resize', reposition);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown, true);
      document.removeEventListener('keydown', onEscape, true);
      document.removeEventListener('scroll', reposition, true);
      window.removeEventListener('resize', reposition);
    };
  }, [open, props.options]);

  useLayoutEffect(() => {
    if (!open || !placement) return;
    const trigger = triggerRef.current;
    const menu = menuRef.current;
    if (!trigger || !menu) return;

    const rect = trigger.getBoundingClientRect();
    const optionWidth = Array.from(
      menu.querySelectorAll<HTMLElement>('.osf-dropdown__option'),
    ).reduce((widest, option) => Math.max(widest, option.scrollWidth), 0);
    const chromeWidth = Math.max(4, menu.offsetWidth - menu.clientWidth + 2);
    const desiredHeight = Math.min(
      MAX_MENU_HEIGHT,
      props.options.length * OPTION_HEIGHT + 2,
    );
    const next = dropdownPlacement(
      rect,
      window.innerWidth,
      window.innerHeight,
      desiredHeight,
      Math.max(rect.width, optionWidth + chromeWidth),
    );

    const menuHeight = menu.getBoundingClientRect().height;
    if (next.opensUp && menuHeight > 0) {
      next.top = Math.max(VIEWPORT_MARGIN, rect.top - MENU_GAP - menuHeight);
    }

    const unchanged = next.opensUp === placement.opensUp
      && Math.abs(next.left - placement.left) < 0.5
      && Math.abs(next.top - placement.top) < 0.5
      && Math.abs(next.width - placement.width) < 0.5
      && Math.abs(next.maxHeight - placement.maxHeight) < 0.5;
    if (!unchanged) setPlacement(next);
  }, [open, placement, props.options]);

  useEffect(() => {
    if (inert && open) close();
  }, [inert, open]);

  const menu = open && placement
    ? (
      <div
        ref={menuRef}
        id={listId}
        class={cx('osf-dropdown__menu', props.menuClass)}
        role="listbox"
        data-nav-modal="1"
        data-placement={placement.opensUp ? 'up' : 'down'}
        aria-label={props.ariaLabel}
        style={{
          left: `${placement.left}px`,
          top: `${placement.top}px`,
          width: `${placement.width}px`,
          maxHeight: `${placement.maxHeight}px`,
        }}
      >
        {props.options.map((option, index) => (
          <div
            key={option.value}
            id={optionId(index)}
            class="osf-dropdown__option"
            role="option"
            aria-selected={index === selected ? 'true' : 'false'}
            aria-disabled={option.disabled ? 'true' : 'false'}
            data-active={index === activeIndex ? 'true' : 'false'}
            onMouseDown={(event) => event.preventDefault()}
            onClick={() => choose(index)}
            onMouseEnter={() => {
              if (!option.disabled) setActiveIndex(index);
            }}
          >
            {option.label}
          </div>
        ))}
      </div>
    )
    : null;

  return (
    <div class={cx('osf-dropdown', props.class)}>
      <button
        ref={triggerRef}
        id={props.id}
        type="button"
        class="osf-select osf-dropdown__trigger"
        role="combobox"
        aria-haspopup="listbox"
        aria-expanded={open ? 'true' : 'false'}
        aria-controls={listId}
        aria-activedescendant={open && activeIndex >= 0 ? optionId(activeIndex) : undefined}
        aria-label={props.ariaLabel}
        disabled={inert}
        onClick={() => (open ? close() : openAt('selected'))}
        onKeyDown={onKeyDown}
      >
        <span class="osf-dropdown__value">{current?.label ?? ''}</span>
        <span class="osf-dropdown__chevron" aria-hidden="true" />
      </button>
      {menu ? createPortal(menu, document.body) : null}
    </div>
  );
}
