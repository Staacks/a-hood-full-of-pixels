// Case for the display board (docs/perfboard_display.svg), a box with a screwed-on lid.
//
// The perfboard is 80 x 60 mm with M2 holes at the corners (75 x 55 apart). It sits on four
// bosses with 3 mm holes for M2 heat-set inserts. The hole grid of the layout is centred on
// the board. Coordinates are those of the top view: x = right (column A -> AA),
// y = up (row 22 -> row 1), z = up from the bed, origin at the board centre.
//
// Openings:
// - left wall: slot for the two ribbon cables (J1, J3) just above the plugs. The wall above
//   it is a tongue on the lid, so with the lid off the notch is open to the top and the
//   cables are laid in with their 2x8 plugs on;
// - front wall (bottom edge of the top view): window for the panel power wires, just above
//   the board surface, big enough for a 2-pole Wago 221 clamp to pass;
// - lid: opening above J2 for the USB-C plug, with a channel to the front edge, so the lid
//   comes off while the cable stays plugged in. The cable lies across the lid, bends over the
//   right edge and runs down the right wall, where a rail takes it with cable ties (strain
//   relief, the plug is not pulled);
// - floor: pairs of slots for cable ties to mount the case (the tie runs inside along the
//   floor, under the board).
//
// Screws: board M2 x 6 (into the floor inserts), lid M3 x 8 (into the post inserts).
//
// Usage:
//   openscad -o display_case.stl display_case.scad
//   openscad -D 'part="lid"' -o display_case_lid.stl display_case.scad   (print orientation)
//   openscad -D 'part="assembly"' -o assembly.png display_case.scad   (with dummy board)

part = "case";             // "case" | "lid" | "assembly"

$fn = 48;
e = 0.01;

// ---- board ------------------------------------------------------------------
pitch = 2.54;
board = [80, 60];
board_t = 1.6;
holes = [75, 55];          // mounting hole spacing
board_h = 52;              // board bottom to the highest part / cable bend
clr = 4;                   // gap board edge -> wall, for the cable ties around the board
standoff = 8;              // floor to board bottom (solder side, tie heads)

function hx(c) = (c - 13) * pitch;      // hole column (A = 0 ... AA = 26) -> x
function hy(r) = (11.5 - r) * pitch;    // hole row (1 ... 22) -> y

// ---- case -------------------------------------------------------------------
wall = 2.4;
floor_t = 2.4;
lid_t = 2.4;
r_out = 3;                 // outer corner radius
top_clr = 3;               // above board_h
cav = [board.x + 2 * clr, board.y + 2 * clr];   // inner size
H = floor_t + standoff + board_h + top_clr;     // height of the walls (lid on top)
zt = floor_t + standoff + board_t;              // top surface of the board

board_insert_d = 3;        // hole for M2 heat-set inserts (board)
board_insert_depth = 5;
boss_d = 6.5;
insert_d = 4.2;            // hole for M3 heat-set inserts (lid)
insert_depth = 7;
post_r = 5;                // lid screw posts in the corners (bulge outwards)
post_xy = [cav.x / 2 + wall / 2, cav.y / 2 + wall / 2];
screw_d = 3.4;

// ribbon cables J1/J3: pins in rows 3-10, both cables leave to the left
rib_y = hy(6.5);
rib_w = 28;                // a 2x8 plug (~26 mm) passes the notch lengthwise
rib_z = [zt + 12, zt + 26];  // slot; the plugs end ~17 mm above the board
tongue_cl = 0.3;           // lid tongue in the notch, per side

// panel power wires (pads C/E 18-20): window in the front wall
pwr_x = hx(3);
pwr_w = 22;
pwr_z = [zt + 1, zt + 15];

// USB-C plug into J2 (columns A-B, rows 18-22) from above
usb_xy = [hx(0.5), hy(20)];
usb_hole = [16, 16];
usb_chan = 7;              // channel from the hole to the front edge

// cable-tie rail for the USB cable on the outside of the right wall
rail_d = 6;                // how far it stands off the wall
rail_w = 12;
rail_top = H - 14;         // room for the bend over the edge
cable_d = 5;               // groove for the cable
tie_w = 5.4;               // cable ties up to 4.8 mm wide
tie_t = 2.2;
tie_z = [10, 25, 40];

// floor slots for mounting the case: [centre, rotation]; the tie goes through one slot of a
// pair, along the floor inside and out through the other
slot_pairs = [[[0, 18], 0], [[0, -18], 0], [[-28, 0], 90], [[28, 0], 90]];
slot_gap = 30;             // centre distance within a pair

// ---- helpers ------------------------------------------------------------------
module rrect(size, r) offset(r) offset(-r) square(size, center = true);

module posts2d() for (sx = [-1, 1], sy = [-1, 1]) translate([sx * post_xy.x, sy * post_xy.y]) circle(post_r);

module outer2d() union() { rrect(cav + 2 * [wall, wall], r_out); posts2d(); }

module cavity2d() difference() { rrect(cav, 1); posts2d(); }

module at_holes() for (sx = [-1, 1], sy = [-1, 1]) translate([sx * holes.x / 2, sy * holes.y / 2, 0]) children();

module at_posts() for (sx = [-1, 1], sy = [-1, 1]) translate([sx * post_xy.x, sy * post_xy.y, 0]) children();

x_left = -(cav.x / 2 + wall);    // outer face of the left wall
x_right = cav.x / 2 + wall;
y_front = -(cav.y / 2 + wall);

// ---- case ---------------------------------------------------------------------
module rail()
    difference() {
        translate([x_right - e, usb_xy.y - rail_w / 2, 0]) cube([rail_d + e, rail_w, rail_top]);
        // groove for the cable
        translate([x_right + rail_d + cable_d / 2 - 1.5, usb_xy.y, -e]) cylinder(d = cable_d, h = rail_top + 2 * e);
        // tie tunnels against the wall
        for (z = tie_z)
            translate([x_right - e, usb_xy.y - rail_w / 2 - e, z - tie_w / 2]) cube([tie_t + e, rail_w + 2 * e, tie_w]);
        // chamfer the top so the bend does not hit an edge
        translate([x_right, usb_xy.y, rail_top]) rotate([90, 0, 0])
            linear_extrude(rail_w + 2, center = true) polygon([[rail_d + e, -rail_d], [rail_d + e, e], [-e, e]]);
    }

module case()
    difference() {
        union() {
            difference() {
                linear_extrude(H) outer2d();
                translate([0, 0, floor_t]) linear_extrude(H) cavity2d();
            }
            at_holes() cylinder(d = boss_d, h = floor_t + standoff);
            rail();
        }
        // inserts: board bosses and lid posts
        at_holes() translate([0, 0, floor_t + standoff - board_insert_depth]) cylinder(d = board_insert_d, h = board_insert_depth + e);
        at_posts() translate([0, 0, H - insert_depth]) cylinder(d = insert_d, h = insert_depth + e);
        // ribbon cables: slot plus the notch above it that the lid tongue fills
        translate([x_left - 1, rib_y - rib_w / 2, rib_z[0]]) cube([wall + 2, rib_w, H - rib_z[0] + e]);
        // power wires
        translate([pwr_x, -cav.y / 2 - wall / 2, (pwr_z[0] + pwr_z[1]) / 2]) rotate([90, 0, 0])
            linear_extrude(wall + 2, center = true) rrect([pwr_w, pwr_z[1] - pwr_z[0]], 2);
        // mounting slots
        for (p = slot_pairs) translate(p[0]) rotate(p[1]) for (sx = [-1, 1])
            translate([sx * slot_gap / 2, 0, -e]) linear_extrude(floor_t + 2 * e) rrect([tie_t, tie_w], 0.5);
    }

// ---- lid ------------------------------------------------------------------------
// lid in assembly position: plate at z = 0 ... lid_t, ribbon tongue hanging down into the notch
module lid()
    difference() {
        union() {
            linear_extrude(lid_t) outer2d();
            translate([x_left, rib_y - rib_w / 2 + tongue_cl, rib_z[1] - H]) cube([wall, rib_w - 2 * tongue_cl, H - rib_z[1] + e]);
        }
        at_posts() translate([0, 0, -e]) cylinder(d = screw_d, h = lid_t + 2 * e);
        translate([0, 0, -e]) linear_extrude(lid_t + 2 * e) {
            translate(usb_xy) rrect(usb_hole, 2);
            translate([usb_xy.x - usb_chan / 2, y_front - 1]) square([usb_chan, usb_xy.y - y_front + 1]);
        }
    }

// ---- dummy board for the assembly view -------------------------------------------
module dummy_board() {
    z0 = floor_t + standoff;
    color("burlywood") translate([0, 0, z0]) difference() {
        translate([-board.x / 2, -board.y / 2, 0]) cube([board.x, board.y, board_t]);
        at_holes() translate([0, 0, -1]) cylinder(d = 2.2, h = 5);
    }
    // J3 and J1 box headers with plugs, ribbons up and out to the left
    for (c = [[6.5, 21], [10.5, 23]]) {
        color("dimgray") translate([hx(c[0]) - 4.5, rib_y - 15.25, zt]) cube([9, 30.5, 9]);
        color("gray") translate([hx(c[0]) - 3.5, rib_y - 13, zt + 9]) cube([7, 26, 8]);
        color("lightsteelblue") {
            translate([hx(c[0]) - 0.6, rib_y - 10.15, zt + 17]) cube([1.2, 20.3, c[1] - 17]);
            translate([x_left - 15, rib_y - 10.15, zt + c[1] - 0.6]) cube([hx(c[0]) - x_left + 15, 20.3, 1.2]);
        }
    }
    color("navy") translate([hx(3), hy(15), zt]) cylinder(d = 13, h = 25);       // C4
    color("darkgreen") translate([hx(21.5) - 10.5, hy(12.5) - 25.5, zt + 8.5]) cube([21, 51, 1]);   // Pico
    color("black") translate([usb_xy.x - 4, usb_xy.y - 5, zt]) cube([8, 10, 10]); // J2
    // USB cable: up through the lid, across it, over the right edge, down the rail
    zc = H + lid_t + 2;
    xc = x_right + rail_d + 1.5;
    color("white") {
        translate([usb_xy.x - 6, usb_xy.y - 3.5, zt + 10]) cube([12, 7, 20]);     // plug
        translate([usb_xy.x, usb_xy.y, zt + 30]) cylinder(d = 4, h = zc - zt - 30);
        translate([usb_xy.x, usb_xy.y, zc]) rotate([0, 90, 0]) cylinder(d = 4, h = xc - usb_xy.x);
        translate([xc, usb_xy.y, -10]) cylinder(d = 4, h = zc + 10);
    }
}

if (part == "case") case();
else if (part == "lid") translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
else if (part == "assembly") {
    color("orange", 0.8) case();
    color("orange", 0.5) translate([0, 0, H]) lid();
    dummy_board();
}
