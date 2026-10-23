// Brackets that join the two printed panel-mount halves at the outer ends of the
// eye slit, replacing the small glue faces of panel_mount.scad.
//
// Each bracket sits behind the slit-side bars of both halves, from the vertical
// rib at x = +-60 to the outer end. The bars drop into two pockets that are an
// exact imprint of them (plus clearance cl): a back plate behind both bars, a lip
// over each bar's inner edge, a tongue filling the slit between the bars, and a
// cap around the outer end. Pushed on from behind it sets the angle between the
// halves; glue it in for strength (the fit holds everything while the glue cures).
//
// The pocket is the bars' shadow along one insertion direction, so the bracket
// slides straight on, and it prints with its flat back on the bed without supports.
// Both brackets are the same part (the left one is the right one turned 180 deg).
//
// Usage:
//   openscad -o joint_bracket.stl joint_bracket.scad                  (print 2x)
//   openscad -D 'part="check_halves"' -o x.stl joint_bracket.scad     (must be empty)
//   openscad -D 'part="check_panels"' -o x.stl joint_bracket.scad     (must be empty)

include <panel_mount.scad>
mode = "none";             // silence panel_mount.scad's own output

part = "print";            // "print" | "assembly" | "check_halves" | "check_panels" | "check_slide"

cl = 0.25;                 // clearance per side between bracket and bars
lip = 8;                   // how far the lips reach over the bars' inner edges
back_t = 3;                // minimum thickness of the back plate
cap = 3;                   // wall around the outer end of the bars
bracket_u0 = 65;           // inner end of the bracket (vertical rib ends at 64)
du = 4;                    // slice width for the convex pieces
L = 60;                    // sweep length for the insertion shadow
slide = 0;                 // check_slide: bracket pulled back by this much

// insertion direction: radial at the bracket's centre, in the joint plane
phi_b = a((bracket_u0 + W / 2 + cap) / 2);
module to_ins() rotate([0, -phi_b, 0]) children();     // insertion direction -> +z
module from_ins() rotate([0, phi_b, 0]) children();

// upper-panel-local (u, v, r) -> assembly coordinates (same as to_upper() to_local())
function gl(u, v, r) = let(y = v + H / 2, z = r * cos(a(u)) - zc)
    [r * sin(a(u)), y * cos(theta) + z * sin(theta), zc + z * cos(theta) - y * sin(theta)];
function ins_z(p) = p[0] * sin(phi_b) + p[2] * cos(phi_b);

v_lip = -H / 2 + bar + lip;
u_end = W / 2 + cap;
z_back = min([for (u = [bracket_u0 : 1 : u_end], v = [-H / 2, v_lip]) ins_z(gl(u, v, rb))]) - back_t;

// convex piece swept along the insertion direction (in insertion frame): the body
// backwards (gives the back plate), the bars forwards (space they slide in through)
module swept_back() hull() { children(); translate([0, 0, -L]) children(); }
module swept_fwd() hull() { children(); translate([0, 0, L]) children(); }

module upper(u0, u1, v0, v1, r0, r1) to_upper() to_local() cbox(u0, u1, v0, v1, r0, r1);

module body_ins()
    intersection() {
        union() for (u = [bracket_u0 : du : u_end - 0.001]) {
            u1 = min(u + du, u_end);
            swept_back() to_ins() {
                upper(u, u1, -H / 2, v_lip, rb, rf);
                mirror([0, 1, 0]) upper(u, u1, -H / 2, v_lip, rb, rf);
            }
        }
        translate([-500, -500, z_back]) cube(1000);
    }

// Upper half's solid parts near the slit end, grown by cl (each piece convex).
module boss_clipped(m)
    to_upper() to_local() intersection() {
        rcyl(m[0], m[1], rb - cl, Rc + cl, boss_d + 2 * cl);
        rotate([0, 0, a(W / 2 + cl)]) translate([-500, -1000, -500]) cube(1000);  // u <= W/2 + cl
        translate([-500, -500, -H / 2 - cl]) cube(1000);                            // v >= -H/2 - cl
    }

module cavity_ins()
    for (m = [0, 1]) mirror([0, m, 0]) {
        for (u = [bracket_u0 - 6 : du : W / 2 - 0.001])
            swept_fwd() to_ins() upper(u - cl, min(u + du, W / 2) + cl, -H / 2 - cl, -H / 2 + bar + cl, rb - cl, rf + cl);
        for (u = [W / 2 - bar : du : W / 2 - 0.001])
            swept_fwd() to_ins() upper(u - cl, u + du + cl, -H / 2 - cl, v_lip + 10, rb - cl, rf + cl);
        // vertical rib at x = 60 (the back plate reaches behind it)
        swept_fwd() to_ins() upper(60 - rib / 2 - cl, 60 + rib / 2 + cl, -H / 2 - cl, v_lip + 10, rb - cl, rf + cl);
        for (mg = magnets) if (mg[1] < 0 && mg[0] > bracket_u0 - boss_d) swept_fwd() to_ins() boss_clipped(mg);
        swept_fwd() to_ins() hull() {
            upper(bridge_u - cl, W / 2 + cl, -H / 2 - cl, -H / 2 + bar + cl, rb - cl, rf + cl);
            mirror([0, 1, 0]) upper(bridge_u - cl, W / 2 + cl, -H / 2 - cl, -H / 2 + bar + cl, rb - cl, rf + cl);
        }
    }

module bracket_ins() difference() { body_ins(); cavity_ins(); }

// both brackets in assembly coordinates
module brackets() {
    from_ins() bracket_ins();
    mirror([1, 0, 0]) from_ins() bracket_ins();   // = the same part turned 180 deg about z
}

module halves() { half_global(); mirror([0, 1, 0]) half_global(); }

if (part == "print") {
    translate([0, 0, -z_back]) bracket_ins();
} else if (part == "assembly") {
    color("orange") half_global();
    color("gold") mirror([0, 1, 0]) half_global();
    color("steelblue") brackets();
} else if (part == "check_halves") {
    intersection() { brackets(); halves(); }
} else if (part == "check_panels") {
    intersection() { brackets(); union() { to_upper() panel_ghost(); mirror([0, 1, 0]) to_upper() panel_ghost(); } }
} else if (part == "check_slide") {
    intersection() { from_ins() translate([0, 0, -slide]) bracket_ins(); halves(); }
}
