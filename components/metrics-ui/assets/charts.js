/* Canvas strip-chart renderer.
 *
 * All channels share one time scale and one crosshair; each channel owns its
 * y scale. Series are drawn as 2px lines over hairline grids. Dense windows
 * are decimated per pixel column keeping min and max, so bursts and spikes
 * survive; runs are split on null values and on explicit stream breaks so a
 * reported gap is visible as a hole, never bridged by interpolation. */
"use strict";

window.MetricsUI = window.MetricsUI || {};

MetricsUI.charts = (() => {
  const MARGIN_LEFT = 58;
  const MARGIN_RIGHT = 12;
  const MARGIN_TOP = 8;

  function lowerBound(points, t) {
    let low = 0;
    let high = points.length;
    while (low < high) {
      const mid = (low + high) >> 1;
      if (points[mid].t < t) low = mid + 1;
      else high = mid;
    }
    return low;
  }

  function niceTicks(min, max, target = 4) {
    const span = max - min;
    if (!(span > 0)) return [min];
    const rawStep = span / target;
    const magnitude = 10 ** Math.floor(Math.log10(rawStep));
    let step = 10 * magnitude;
    for (const unit of [1, 2, 2.5, 5, 10]) {
      if (unit * magnitude >= rawStep) {
        step = unit * magnitude;
        break;
      }
    }
    const ticks = [];
    for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-9; v += step) {
      ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
    }
    return ticks;
  }

  const TIME_STEPS_MS = [
    1000, 2000, 5000, 10000, 15000, 30000,
    60000, 120000, 300000, 600000, 900000, 1800000,
    3600000, 7200000,
  ];

  function timeTicks(t0, t1) {
    const span = t1 - t0;
    let step = TIME_STEPS_MS[TIME_STEPS_MS.length - 1];
    for (const candidate of TIME_STEPS_MS) {
      if (span / candidate <= 6.5) {
        step = candidate;
        break;
      }
    }
    const ticks = [];
    for (let t = Math.ceil(t0 / step) * step; t <= t1; t += step) {
      ticks.push(t);
    }
    return ticks;
  }

  // Split [i0, i1] into runs of consecutive points where `key` is non-null,
  // breaking on stream gaps, then reduce each run to scaled vertices with at
  // most two points (the column's min and max, in arrival order) per pixel.
  function buildRuns(points, i0, i1, key, xOf) {
    const runs = [];
    let run = null;
    for (let i = i0; i <= i1; i += 1) {
      const point = points[i];
      if (point.brk && run) {
        runs.push(run);
        run = null;
      }
      const value = point[key];
      if (value === null || value === undefined) {
        if (run) runs.push(run);
        run = null;
        continue;
      }
      if (!run) run = [];
      run.push(i);
    }
    if (run) runs.push(run);

    return runs.map((indexes) => {
      const vertices = [];
      let columnX = NaN;
      let first = 0;
      let minV = 0;
      let maxV = 0;
      let minAt = 0;
      let maxAt = 0;
      const flush = () => {
        if (Number.isNaN(columnX)) return;
        if (minAt === maxAt) {
          vertices.push([columnX, minV]);
        } else if (minAt < maxAt) {
          vertices.push([columnX, minV], [columnX, maxV]);
        } else {
          vertices.push([columnX, maxV], [columnX, minV]);
        }
        void first;
      };
      for (const i of indexes) {
        const value = points[i][key];
        const x = Math.round(xOf(points[i].t));
        if (x !== columnX) {
          flush();
          columnX = x;
          first = i;
          minV = value;
          maxV = value;
          minAt = i;
          maxAt = i;
        } else {
          if (value < minV) {
            minV = value;
            minAt = i;
          }
          if (value > maxV) {
            maxV = value;
            maxAt = i;
          }
        }
      }
      flush();
      return vertices;
    });
  }

  class StripChart {
    /* spec: {
     *   height, drawXLabels,
     *   yDomain: "auto" | [min, max],
     *   includeZero, yTicks, yFormat,
     *   series: [
     *     { type: "line", key, color, label } |
     *     { type: "dots", key, changedKey, color } |
     *     { type: "diverging", key, posColor, negColor },
     *   ],
     * } */
    constructor(canvas, spec) {
      this.canvas = canvas;
      this.spec = spec;
      this.ctx = canvas.getContext("2d");
      this.cssWidth = 0;
      this.cssHeight = spec.height;
      canvas.style.height = `${spec.height}px`;
    }

    layout() {
      const dpr = window.devicePixelRatio || 1;
      this.cssWidth = this.canvas.clientWidth;
      this.canvas.width = Math.round(this.cssWidth * dpr);
      this.canvas.height = Math.round(this.cssHeight * dpr);
      this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    plotBottom() {
      return this.cssHeight - (this.spec.drawXLabels ? 22 : 8);
    }

    yDomain(points, i0, i1) {
      if (Array.isArray(this.spec.yDomain)) return this.spec.yDomain;
      let min = Infinity;
      let max = -Infinity;
      for (const series of this.spec.series) {
        if (series.type === "dots") continue;
        for (let i = i0; i <= i1; i += 1) {
          const value = points[i][series.key];
          if (value === null || value === undefined) continue;
          if (value < min) min = value;
          if (value > max) max = value;
        }
      }
      if (min === Infinity) return [0, 1];
      if (this.spec.includeZero) min = Math.min(min, 0);
      if (min === max) {
        const pad = Math.max(Math.abs(min) * 0.005, 1);
        return [min - pad, max + pad];
      }
      const pad = (max - min) * 0.07;
      return [
        this.spec.includeZero && min === 0 ? 0 : min - pad,
        max + pad,
      ];
    }

    render(frame) {
      // frame: {points, i0, i1, t0, t1, theme, crossIndex, message}
      const { ctx } = this;
      const { points, i0, i1, t0, t1, theme } = frame;
      const width = this.cssWidth;
      const height = this.cssHeight;
      const bottom = this.plotBottom();
      const plotW = width - MARGIN_LEFT - MARGIN_RIGHT;

      ctx.clearRect(0, 0, width, height);
      if (plotW <= 10) return;

      const hasData = i1 >= i0 && points.length > 0;
      const xOf = (t) =>
        MARGIN_LEFT + ((t - t0) / Math.max(t1 - t0, 1)) * plotW;

      const [yMin, yMax] = hasData
        ? this.yDomain(points, i0, i1)
        : Array.isArray(this.spec.yDomain)
          ? this.spec.yDomain
          : [0, 1];
      const yOf = (v) =>
        bottom - ((v - yMin) / Math.max(yMax - yMin, 1e-12)) * (bottom - MARGIN_TOP);

      // Grid + y labels.
      const yTicks = this.spec.yTicks || niceTicks(yMin, yMax);
      ctx.font = '10.5px ui-monospace, Menlo, Consolas, monospace';
      ctx.lineWidth = 1;
      for (const tick of yTicks) {
        if (tick < yMin - 1e-9 || tick > yMax + 1e-9) continue;
        const y = Math.round(yOf(tick)) + 0.5;
        ctx.strokeStyle = tick === 0 && yMin < 0 ? theme.baseline : theme.grid;
        ctx.beginPath();
        ctx.moveTo(MARGIN_LEFT, y);
        ctx.lineTo(width - MARGIN_RIGHT, y);
        ctx.stroke();
        ctx.fillStyle = theme.muted;
        ctx.textAlign = "right";
        ctx.textBaseline = "middle";
        ctx.fillText(this.spec.yFormat(tick), MARGIN_LEFT - 8, y);
      }

      // Time grid, labels only on the bottom channel.
      for (const tick of timeTicks(t0, t1)) {
        const x = Math.round(xOf(tick)) + 0.5;
        ctx.strokeStyle = theme.grid;
        ctx.beginPath();
        ctx.moveTo(x, MARGIN_TOP);
        ctx.lineTo(x, bottom);
        ctx.stroke();
        if (this.spec.drawXLabels) {
          ctx.fillStyle = theme.muted;
          ctx.textAlign = "center";
          ctx.textBaseline = "top";
          ctx.fillText(MetricsUI.format.simTick(tick), x, bottom + 6);
        }
      }

      // Plot baseline.
      ctx.strokeStyle = theme.baseline;
      ctx.beginPath();
      ctx.moveTo(MARGIN_LEFT, Math.round(bottom) + 0.5);
      ctx.lineTo(width - MARGIN_RIGHT, Math.round(bottom) + 0.5);
      ctx.stroke();

      if (!hasData) {
        ctx.fillStyle = theme.muted;
        ctx.font = '12px system-ui, sans-serif';
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(
          frame.message || "waiting for metrics…",
          MARGIN_LEFT + plotW / 2,
          (MARGIN_TOP + bottom) / 2
        );
        return;
      }

      ctx.save();
      ctx.beginPath();
      ctx.rect(MARGIN_LEFT, MARGIN_TOP, plotW, bottom - MARGIN_TOP);
      ctx.clip();

      for (const series of this.spec.series) {
        if (series.type === "line") {
          this.drawLines(frame, series, xOf, yOf, theme.color(series.color));
        } else if (series.type === "diverging") {
          this.drawDiverging(frame, series, xOf, yOf, theme);
        } else if (series.type === "dots") {
          this.drawDots(frame, series, xOf, yOf, theme.color(series.color));
        }
      }

      this.drawCrosshair(frame, xOf, yOf);
      ctx.restore();
    }

    drawLines(frame, series, xOf, yOf, color) {
      const { ctx } = this;
      const runs = buildRuns(frame.points, frame.i0, frame.i1, series.key, xOf);
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.lineJoin = "round";
      ctx.lineCap = "round";
      for (const vertices of runs) {
        if (vertices.length === 1) {
          ctx.fillStyle = color;
          ctx.beginPath();
          ctx.arc(vertices[0][0], yOf(vertices[0][1]), 1.5, 0, Math.PI * 2);
          ctx.fill();
          continue;
        }
        ctx.beginPath();
        vertices.forEach(([x, v], index) => {
          if (index === 0) ctx.moveTo(x, yOf(v));
          else ctx.lineTo(x, yOf(v));
        });
        ctx.stroke();
      }
      if (series.fillToZero) {
        ctx.fillStyle = color;
        ctx.globalAlpha = 0.1;
        for (const vertices of runs) {
          if (vertices.length < 2) continue;
          ctx.beginPath();
          vertices.forEach(([x, v], index) => {
            if (index === 0) ctx.moveTo(x, yOf(v));
            else ctx.lineTo(x, yOf(v));
          });
          ctx.lineTo(vertices[vertices.length - 1][0], yOf(0));
          ctx.lineTo(vertices[0][0], yOf(0));
          ctx.closePath();
          ctx.fill();
        }
        ctx.globalAlpha = 1;
      }
      if (series.endDot && runs.length) {
        const lastRun = runs[runs.length - 1];
        const [x, v] = lastRun[lastRun.length - 1];
        ctx.beginPath();
        ctx.arc(x, yOf(v), 4, 0, Math.PI * 2);
        ctx.fillStyle = color;
        ctx.fill();
        ctx.lineWidth = 2;
        ctx.strokeStyle = frame.theme.surface;
        ctx.stroke();
      }
    }

    // Signed series: the positive lobe wears one entity color, the negative
    // lobe the other, split exactly at interpolated zero crossings.
    drawDiverging(frame, series, xOf, yOf, theme) {
      const { ctx } = this;
      const runs = buildRuns(frame.points, frame.i0, frame.i1, series.key, xOf);
      const posColor = theme.color(series.posColor);
      const negColor = theme.color(series.negColor);
      const zeroY = yOf(0);

      for (const vertices of runs) {
        if (vertices.length < 2) continue;
        let segment = [vertices[0]];
        const segments = [];
        for (let i = 1; i < vertices.length; i += 1) {
          const [x0, v0] = vertices[i - 1];
          const [x1, v1] = vertices[i];
          if ((v0 >= 0) !== (v1 >= 0)) {
            const ratio = v0 === v1 ? 0 : v0 / (v0 - v1);
            const crossX = x0 + (x1 - x0) * ratio;
            segment.push([crossX, 0]);
            segments.push(segment);
            segment = [[crossX, 0]];
          }
          segment.push(vertices[i]);
        }
        segments.push(segment);

        for (const part of segments) {
          const sign = part.find(([, v]) => v !== 0)?.[1] ?? 0;
          const color = sign >= 0 ? posColor : negColor;
          ctx.fillStyle = color;
          ctx.globalAlpha = 0.1;
          ctx.beginPath();
          part.forEach(([x, v], index) => {
            if (index === 0) ctx.moveTo(x, yOf(v));
            else ctx.lineTo(x, yOf(v));
          });
          ctx.lineTo(part[part.length - 1][0], zeroY);
          ctx.lineTo(part[0][0], zeroY);
          ctx.closePath();
          ctx.fill();
          ctx.globalAlpha = 1;
          ctx.strokeStyle = color;
          ctx.lineWidth = 2;
          ctx.lineJoin = "round";
          ctx.lineCap = "round";
          ctx.beginPath();
          part.forEach(([x, v], index) => {
            if (index === 0) ctx.moveTo(x, yOf(v));
            else ctx.lineTo(x, yOf(v));
          });
          ctx.stroke();
        }
      }
    }

    // Trade prints: one small mark per point where `changedKey` advanced.
    // These are texture behind the lines — values live in the readout row.
    drawDots(frame, series, xOf, yOf, color) {
      const { ctx } = this;
      const { points, i0, i1 } = frame;
      ctx.fillStyle = color;
      ctx.globalAlpha = 0.55;
      let lastX = -Infinity;
      for (let i = Math.max(i0, 1); i <= i1; i += 1) {
        const point = points[i];
        const value = point[series.key];
        if (value === null || value === undefined) continue;
        if (point[series.changedKey] === points[i - 1][series.changedKey]) {
          continue;
        }
        const x = xOf(point.t);
        if (x - lastX < 2.5) continue;
        lastX = x;
        ctx.beginPath();
        ctx.arc(x, yOf(value), 2, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.globalAlpha = 1;
    }

    drawCrosshair(frame, xOf, yOf) {
      const { ctx } = this;
      if (frame.crossIndex === null || frame.crossIndex === undefined) return;
      const point = frame.points[frame.crossIndex];
      if (!point || point.t < frame.t0 || point.t > frame.t1) return;
      const x = Math.round(xOf(point.t)) + 0.5;
      ctx.strokeStyle = frame.theme.baseline;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, MARGIN_TOP);
      ctx.lineTo(x, this.plotBottom());
      ctx.stroke();
      for (const series of this.spec.series) {
        if (series.type !== "line" && series.type !== "diverging") continue;
        const value = point[series.key];
        if (value === null || value === undefined) continue;
        const color =
          series.type === "line"
            ? frame.theme.color(series.color)
            : frame.theme.color(value >= 0 ? series.posColor : series.negColor);
        ctx.beginPath();
        ctx.arc(x - 0.5, yOf(value), 3.5, 0, Math.PI * 2);
        ctx.fillStyle = color;
        ctx.fill();
        ctx.lineWidth = 2;
        ctx.strokeStyle = frame.theme.surface;
        ctx.stroke();
      }
    }
  }

  return { StripChart, lowerBound };
})();
