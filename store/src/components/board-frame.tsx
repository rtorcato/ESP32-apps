import type { CSSProperties } from "react"
import type { Board } from "@/data/boards"
import { frameSize } from "@/data/boards"

// Port of dev() in Zoey's hardware.html: a vector drawing of the board at `scale`
// px per mm, with the app's preview dropped into the screen. `src` null = blank screen.

const SKIN: Record<string, CSSProperties> = {
  bar: { background: "linear-gradient(#20242B,#14171C)" },
  pcb: { background: "#E9B824", boxShadow: "inset 0 0 0 1px #C99A12" },
  tab: { background: "#111318" },
  "knob-dark": {
    borderRadius: "50%",
    background:
      "conic-gradient(from 20deg,#3b3f47,#1c1f24,#4a4f58,#1c1f24,#3b3f47,#23262c,#3b3f47)",
  },
  "knob-silver": {
    borderRadius: "50%",
    background:
      "conic-gradient(from 10deg,#e6e9ee,#9aa1ab,#f4f6f9,#8d949e,#e6e9ee,#b3b9c2,#e6e9ee)",
  },
}
const RADIUS_MM = { bar: 2.6, pcb: 1.4, tab: 3 }
const GLASS = "#05070B"

export function BoardFrame({
  board,
  src,
  scale,
  alt = "",
  shadow = true,
}: {
  board: Board
  src: string | null
  scale: number
  alt?: string
  shadow?: boolean
}) {
  const f = board.frame
  const px = (mm: number) => mm * scale
  const box = (x: number, y: number, w: number, h: number): CSSProperties => ({
    position: "absolute",
    left: px(x),
    top: px(y),
    width: px(w),
    height: px(h),
  })
  const size = frameSize(f)
  const parts = []

  if ("d" in f) {
    const glass = f.screen + 4
    const o = (f.d - glass) / 2
    const i = (f.d - f.screen) / 2
    if (f.kind === "knob-silver")
      parts.push(
        <div
          key="glow"
          style={{
            ...box(o - 1, o - 1, glass + 2, glass + 2),
            borderRadius: "50%",
            boxShadow:
              "0 0 0 2px rgba(80,140,255,.65),0 0 14px rgba(80,140,255,.5)",
          }}
        />
      )
    parts.push(
      <div
        key="glass"
        style={{
          ...box(o, o, glass, glass),
          borderRadius: "50%",
          background: GLASS,
        }}
      />
    )
    if (src)
      parts.push(
        <img
          key="app"
          src={src}
          alt={alt}
          style={{
            ...box(i, i, f.screen, f.screen),
            borderRadius: "50%",
            objectFit: "cover",
          }}
        />
      )
  } else {
    const s = f.screen
    parts.push(
      <div
        key="glass"
        style={{
          ...box(s.x - 1, s.y - 1, s.w + 2, s.h + 2),
          borderRadius: px(0.8),
          background: GLASS,
        }}
      />
    )
    if (src)
      // object-position left: a few previews (host-monitor) show two layouts side by side.
      parts.push(
        <img
          key="app"
          src={src}
          alt={alt}
          style={{
            ...box(s.x, s.y, s.w, s.h),
            objectFit: "cover",
            objectPosition: "left top",
          }}
        />
      )
    if (f.kind === "pcb") {
      for (const [x, y] of [
        [2.5, 2],
        [47.5, 2],
        [2.5, 84],
        [47.5, 84],
      ])
        parts.push(
          <div
            key={`hole${x}${y}`}
            style={{
              ...box(x - 1.5, y - 1.5, 3, 3),
              borderRadius: "50%",
              background: "#F3F5F8",
              boxShadow: "0 0 0 2px #d8d0b0",
            }}
          />
        )
      parts.push(
        <div
          key="sd"
          style={{
            ...box(8, 66, 16, 18),
            background: "#2a2a2a",
            borderRadius: 2,
          }}
        />,
        <div
          key="usb"
          style={{
            ...box(30, 68, 10, 6),
            background: "#c0c4ca",
            borderRadius: 2,
          }}
        />,
        <div
          key="led"
          style={{
            ...box(30, 77, 5, 5),
            background: "#555",
            borderRadius: "50%",
          }}
        />
      )
    }
    if (f.kind === "bar")
      parts.push(
        <div
          key="btn"
          style={{
            ...box(6, 41, 8, 3),
            background: "#9aa1ab",
            borderRadius: px(1.5),
          }}
        />
      )
  }

  return (
    <div
      className="relative flex-none"
      style={{
        width: px(size.w),
        height: px(size.h),
        borderRadius: "d" in f ? undefined : px(RADIUS_MM[f.kind]),
        filter: shadow
          ? "drop-shadow(0 18px 24px rgba(13,21,38,.18)) drop-shadow(0 2px 4px rgba(13,21,38,.12))"
          : undefined,
        ...SKIN[f.kind],
      }}
    >
      {parts}
    </div>
  )
}
