# Mask geometry (for video content and `convert_video.py`)

The geometry the panel mount (`3d print/mask/panel_mount.scad`) is built for. These are design
values, not measurements of the finished mask: the bend radius is "about 145 mm", and `eye_gap`
is a parameter in the .scad file. If either changes, recompute the numbers with the formulas
below.

## Panels

- Two 128x64 panels, 240 x 120 mm each, so the pixel pitch is p = 240/128 = 120/64 = **1.875 mm**
  in both directions.
- Each panel is bent around its short (vertical) axis to a cylinder of radius **R = 145 mm**
  (measured at the PCB). The long side is curved and the short side stays straight. The 240 mm
  arc covers 2·φmax = 94.8° (φmax = 120/145 rad = 47.4°).
- The panels are stacked: panel U on top, panel L below. They are **not** on one common
  cylinder. U is tilted top-back and L bottom-back, each by **θ = 12.3°** about a horizontal
  left-right axis through the outer corners. The panels touch at their left and right ends and
  leave an **eye slit** between them that is **20 mm wide at the centre** and closes to 0 at the
  ends.

## Coordinates

All of this is from a viewer looking at the mask from the front. Column c = 0 is on the viewer's
left, which is the wearer's right. Row r = 0 is the top row of each panel. Which physical panel is
on top, and whether it is rotated, is a firmware config matter (see [design.md](design.md)) and is not covered
here.

- X = right, Y = up, Z = towards the viewer (away from the face), in mm.
- Y = 0 is the plane in which the two panels touch, at the height of the eye slit.
- The hinge line (outer corners) is at Y = 0, Z = zc = R·cos φmax = 98.1 mm, X = ±106.8 mm.

## Pixel → 3D position

Panel U, pixel (c, r), with c = 0…127 and r = 0…63:

```
u  = (c - 63.5) * p              arc length from the panel centre
v  = (31.5 - r) * p + 60         height above the panel's bottom edge (0.94 … 119.06)
x  = R sin(u/R)
z0 = R cos(u/R) - zc             depth relative to the hinge line
X  = x
Y  = v cos θ + z0 sin θ
Z  = zc + z0 cos θ - v sin θ
```

Panel L is the mirror image: pixel (c, r) of L sits at (X, −Y, Z) of pixel (c, 63 − r) of U.

with p = 1.875, R = 145, zc = 98.115, θ = 12.315° (sin θ = 0.2133, cos θ = 0.9770).

Gap between the panel edges in column c (equivalently at arc position u):

```
gap(u) = 2 sin θ · R · (cos(u/R) - cos φmax)       20.0 mm at u = 0, 0 at u = ±120
```

`θ = asin(eye_gap / 2 / (R - zc))`, where R − zc = 46.9 mm.

## What this means for the video

1. **The gap is lens-shaped, not a fixed number of rows.** The `--gap-rows N` approach (drop N
   uniform middle rows) is only right at the centre column.
2. **The whole image is bowed, not just the gap.** The tilt shifts every row of U up by
   z0·sin θ. That shift is 0 at the outer columns and +10 mm (5.3 rows) at the centre. L is
   shifted down by the same amount. Seen from the front, a straight horizontal line in the source
   comes out as an arc: the upper panel's rows rise towards the middle, the lower panel's rows
   sag. In fact the gap *is* this bowing: gap(c) = 2·shift(c).
3. **Horizontal foreshortening.** Seen from straight ahead, column c sits at X = R sin(u/R).
   That is linear near the centre, but the outer columns are compressed: at the edge each column
   covers about cos 47° ≈ 0.68 of the width it covers at the centre. The total visible width is
   212 mm, not 240 mm.
4. **Vertical scale** within a panel is cos θ = 0.977, which is negligible.

A conversion that gets all of this right samples the source frame, for every panel pixel, at
that pixel's frontal position (X, Y). In other words, it projects the face orthographically onto
the mask, which makes the gap, the bowing and the foreshortening fall out automatically. A
simpler version applies only the per-column vertical shift (points 1 and 2) and ignores the
X foreshortening.

## Reference table (centre of the gap rows)

Half of the mask; it is symmetric about c = 63.5. "Gap" is between the panel edges; between the
pixel centres of U row 63 and L row 0 there is 1 pitch more. Y values are pixel centres.

| column c | X (mm) | gap (mm) | gap (rows) | Y of U row 63 | Y of U row 0 | Z of U row 63 | Z of U row 0 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0    | −106.1 | 0.3  | 0.2  | 1.1  | 116.5 | 98.6  | 73.4  |
| 8    | −95.4  | 4.7  | 2.5  | 3.3  | 118.7 | 108.8 | 83.6  |
| 16   | −83.6  | 8.7  | 4.6  | 5.3  | 120.7 | 117.8 | 92.6  |
| 24   | −70.9  | 12.1 | 6.5  | 7.0  | 122.4 | 125.6 | 100.4 |
| 32   | −57.4  | 14.9 | 8.0  | 8.4  | 123.8 | 132.1 | 106.9 |
| 40   | −43.4  | 17.2 | 9.2  | 9.5  | 124.9 | 137.2 | 112.0 |
| 48   | −28.9  | 18.8 | 10.0 | 10.3 | 125.7 | 140.9 | 115.7 |
| 56   | −14.0  | 19.7 | 10.5 | 10.8 | 126.2 | 143.1 | 117.9 |
| 63.5 | 0.0    | 20.0 | 10.7 | 10.9 | 126.3 | 143.7 | 118.5 |

- Overall size seen from the front: about 212 mm wide and 253 mm tall (pixel centres
  Y = −126.3 … +126.3 at the centre column, ±116.5 at the outer columns).
- The wearer's eyes are at about X = ±32 mm, which is column ≈ 46 and ≈ 81. The gap there is
  18.5 mm, about 9.9 rows. Put the face's eyes just above or around the slit at those columns.
