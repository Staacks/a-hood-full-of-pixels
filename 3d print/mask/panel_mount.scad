// Mount for the two curved 128x64 HUB75 panels of the mask.
//
// One part per panel, the same STL printed twice (the lower half is the upper
// one rotated 180 degrees). Each half is a lightweight curved frame with pockets
// for stacks of disc magnets that match the 12 magnets on the panel back, and an
// opening for the power/data connectors.
//
// Geometry: each panel is bent around its short (y) axis to radius R. The upper
// panel is tilted top-back and the lower one bottom-back about the x axis, so
// they touch at their outer corners and leave an eye slit that is eye_gap wide
// in the middle. Near the outer corners both halves carry a bridge with a flat
// face in the joint plane; glue the two halves together there (optional
// filament dowels) and the angle between them is fixed.
//
// Coordinates (assembly): x = right, y = up, z = forward (away from the face).
// The panel cylinder axis is the y axis; the joint plane between halves is y = 0.
//
// Usage:
//   openscad -o panel_mount_half.stl panel_mount.scad                (print part)
//   openscad -D 'mode="assembly"' -o assembly.png panel_mount.scad   (preview)

mode = "print";            // "print" | "half" | "assembly" | "collision"

// ---- panel ---------------------------------------------------------------
W = 240;                   // panel size along the curved (x) axis
H = 120;                   // panel size along the straight (y) axis
R = 145;                   // bend radius of the panel PCB
standoff = 5;              // PCB to contact face of the panel's magnets (measure!)
eye_gap = 20;              // slit width between the panels at the centre (x = 0)

magnets = [[-113, 53], [-60, 53], [0, 53], [60, 53], [113, 53],
           [-113, 0], [113, 0],
           [-113, -53], [-60, -53], [0, -53], [60, -53], [113, -53]];
conn = [60, 30];           // connector opening (x, y), centred on the panel

// ---- own magnets (stack of 3 discs, 10 x 1.8 mm) ---------------------------
pocket_d = 10.4;           // hole for 10 mm magnets incl. FDM shrink and glue
pocket_depth = 5.2;        // 3 x 1.8 = 5.4 -> magnets stand 0.2 mm proud

// ---- frame ----------------------------------------------------------------
depth = 7;                 // radial thickness of frame and bosses
recess = 1;                // frame front sits this far behind the magnet faces
bar = 16;                  // width of the outer frame bars
ring = 6;                  // wall width around the connector opening
rib = 8;                   // width of the inner ribs
boss_d = 16;               // raised pad around every magnet pocket
bridge_u = 96;             // bridges between the halves span |x| = bridge_u .. W/2
dowel_d = 2.0;             // hole for a piece of 1.75 mm filament
dowel_depth = 8;
dowel_u = 101;

$fa = 1;
$fs = 0.5;

// ---- derived --------------------------------------------------------------
Rc = R - standoff;         // radius of the magnet contact face
rb = Rc - depth;           // back of the frame
rf = Rc - recess;          // front of the frame
phi_max = (W / 2) / R * 180 / PI;
zc = R * cos(phi_max);     // hinge line (outer corners) lies at y = 0, z = zc
theta = asin(eye_gap / 2 / (R - zc));

function a(u) = u / R * 180 / PI;   // arc length on the PCB -> angle

echo(str("tilt per panel = ", theta, " deg, hinge z = ", zc));

// ---- panel-local building blocks -------------------------------------------
// Built with the cylinder axis on Z ("cyl frame"); to_local() maps it to the
// panel-local frame (axis on y, panel centre at (0, 0, R)).
module to_local() multmatrix([[0, 1, 0, 0], [0, 0, 1, 0], [1, 0, 0, 0], [0, 0, 0, 1]]) children();

// Cylindrical box: arc range u0..u1 (measured on the PCB), height v0..v1,
// radius r0..r1.
module cbox(u0, u1, v0, v1, r0, r1)
    rotate([0, 0, a(u0)]) rotate_extrude(angle = a(u1) - a(u0))
        translate([r0, v0]) square([r1 - r0, v1 - v0]);

// Radial cylinder at (u, v) from radius r0 to r1.
module rcyl(u, v, r0, r1, dia)
    rotate([0, 0, a(u)]) translate([r0, 0, v]) rotate([0, 90, 0]) cylinder(h = r1 - r0, d = dia);

module frame_solid() {
    intersection() {
        cbox(-W / 2, W / 2, -H / 2, H / 2, rb - 1, Rc + 1);   // clip to panel outline
        union() {
            // outer frame (all magnets sit on it)
            cbox(-W / 2, W / 2, H / 2 - bar, H / 2, rb, rf);
            cbox(-W / 2, W / 2, -H / 2, -H / 2 + bar, rb, rf);
            cbox(W / 2 - bar, W / 2, -H / 2, H / 2, rb, rf);
            cbox(-W / 2, -W / 2 + bar, -H / 2, H / 2, rb, rf);
            // ring around the connector opening
            cbox(-conn[0] / 2 - ring, conn[0] / 2 + ring, -conn[1] / 2 - ring, conn[1] / 2 + ring, rb, rf);
            // ribs: centre top/bottom, horizontal to the sides, verticals at x = +-60
            cbox(-rib / 2, rib / 2, -H / 2, H / 2, rb, rf);
            cbox(-W / 2, W / 2, -rib / 2, rib / 2, rb, rf);
            for (s = [-1, 1]) cbox(s * 60 - rib / 2, s * 60 + rib / 2, -H / 2, H / 2, rb, rf);
            for (m = magnets) rcyl(m[0], m[1], rb, Rc, boss_d);
        }
    }
}

module frame_cuts() {
    for (m = magnets) rcyl(m[0], m[1], Rc - pocket_depth, Rc + 1, pocket_d);
    cbox(-conn[0] / 2, conn[0] / 2, -conn[1] / 2, conn[1] / 2, rb - 1, Rc + 1);
}

// ---- assembly transforms ---------------------------------------------------
// Upper panel: bottom edge on y = 0, then tilted top-back about the hinge line.
module to_upper()
    translate([0, 0, zc]) rotate([-theta, 0, 0]) translate([0, 0, -zc]) translate([0, H / 2, 0]) children();
module from_upper()
    translate([0, -H / 2, 0]) translate([0, 0, zc]) rotate([theta, 0, 0]) translate([0, 0, -zc]) children();

module end_piece(s)
    to_local() if (s > 0) cbox(bridge_u, W / 2, -H / 2, -H / 2 + bar, rb, rf);
               else       cbox(-W / 2, -bridge_u, -H / 2, -H / 2 + bar, rb, rf);

module bridges()
    for (s = [-1, 1]) hull() {
        to_upper() end_piece(s);
        mirror([0, 1, 0]) to_upper() end_piece(s);
    }

module dowel_holes()
    for (s = [-1, 1]) {
        r = (rb + rf) / 2;
        x0 = s * r * sin(a(dowel_u));
        z0 = zc + (r * cos(a(dowel_u)) - zc) * cos(theta);
        translate([x0, -1, z0]) rotate([-90, 0, 0]) cylinder(h = dowel_depth + 1, d = dowel_d);
    }

// Upper half in assembly coordinates; the lower half is mirror([0, 1, 0]) of it.
module half_global()
    difference() {
        intersection() {
            union() {
                to_upper() to_local() frame_solid();
                bridges();
            }
            translate([-200, 0, -100]) cube([400, 300, 400]);
        }
        to_upper() to_local() frame_cuts();
        dowel_holes();
    }

module panel_ghost()
    to_local() difference() {
        union() {
            cbox(-W / 2, W / 2, -H / 2, H / 2, R, R + 2);
            for (m = magnets) rcyl(m[0], m[1], Rc + 0.01, R, 10);
        }
        cbox(-conn[0] / 2 + 5, conn[0] / 2 - 5, -conn[1] / 2 + 5, conn[1] / 2 - 5, R - 1, R + 3);
    }

if (mode == "print") {
    // top edge of the panel on the bed, eye-slit edge up
    translate([0, 0, H / 2]) rotate([-90, 0, 0]) from_upper() half_global();
} else if (mode == "half") {
    half_global();
} else if (mode == "assembly") {
    color("orange") half_global();
    color("gold") mirror([0, 1, 0]) half_global();
    color("dimgray", 0.6) to_upper() panel_ghost();
    color("dimgray", 0.6) mirror([0, 1, 0]) to_upper() panel_ghost();
} else if (mode == "collision") {
    // must be empty: mount vs. both panels
    intersection() {
        union() { half_global(); mirror([0, 1, 0]) half_global(); }
        union() { to_upper() panel_ghost(); mirror([0, 1, 0]) to_upper() panel_ghost(); }
    }
}
