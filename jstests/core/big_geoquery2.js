//
//  this test is related to the Big Polygon (multi-hemisphere)
//  GEO query feature in mongod
//  see:  https://jira.mongodb.org/browse/CAP-1099
//  and:  https://jira.mongodb.org/browse/SERVER-14510
//
// 4.2.1.1 big poly parsing
//

var coll = db.big_geodata;
//coll.getMongo().getDB("admin").runCommand({ setParameter : 1, help: true})
coll.getMongo().getDB("admin").runCommand({ setParameter : 1, logLevel: 1})


// 4.2.1.1.1 wrong CRS
// this CRS string no good but referenced at
// https://wiki.mongodb.com/display/10GEN/Multi-hemisphere+%28BigPolygon%29+queries
var badCRS = {
    type: 'name',
    properties: {
        name: 'urn:mongodb:crs:strictwinding:EPSG:4326'
    }
};
var poly = {
    type: 'Polygon',
    coordinates: [
        [
            [ 114.0834046, 22.6648202 ],
            [ 113.8293457, 22.3819359 ],
            [ 114.2736054, 22.4047911 ],
            [ 114.0834046, 22.6648202 ]
        ]
    ],
    crs: badCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with bad CRS');



// good CRS
goodCRS= {
    type: 'name',
    properties: {
        name: 'urn:mongodb:strictwindingcrs:EPSG:4326'
    }
};

// 4.2.1.1.2 open polygon
poly = {
    type: 'Polygon',
    coordinates: [
        [
            [ 114.0834046, 22.6648202 ],
            [ 113.8293457, 22.3819359 ]
        ]
    ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with open polygon < 3');



// 4.2.1.1.3 open polygon > 3 vertices
poly = {
    type: 'Polygon',
    coordinates: [
        [
            [ 114.0834046, 22.6648202 ],
            [ 113.8293457, 22.3819359 ],
            [ 114.2736054, 22.4047911 ],
            [ 114.1, 22.5 ]
        ]
    ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with open polygon > 3');



// 4.2.1.1.4 duplicate non-adj points
poly = {
    type: 'Polygon',
    coordinates: [
        [
            [ 114.0834046, 22.6648202 ],
            [ 113.8293457, 22.3819359 ],
            [ 114.2736054, 22.4047911 ],
            [ -65.9165954, 22.6648202 ]
        ]
    ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with duplicate non-adjacent points');


// 4.2.1.1.5 one hole
poly = {
    type: 'Polygon',
    coordinates: [
              [ [ -80.0, 30.0 ], [ -40.0, 30.0 ], [ -40.0, 60.0 ], [-80.0, 60.0 ], [ -80.0, 30.0 ] ],
              [ [ -70.0, 40.0 ], [ -60.0, 40.0 ], [ -60.0, 50.0 ], [-70.0, 50.0 ], [ -70.0, 40.0 ] ]
    ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with one hole');


// 4.2.1.1.5 two holes
poly = {
    type: 'Polygon',
    coordinates: [
              [ [ -80.0, 30.0 ], [ -40.0, 30.0 ], [ -40.0, 60.0 ], [-80.0, 60.0 ], [ -80.0, 30.0 ] ],
              [ [ -70.0, 40.0 ], [ -60.0, 40.0 ], [ -60.0, 50.0 ], [-70.0, 50.0 ], [ -70.0, 40.0 ] ],
              [ [ -55.0, 40.0 ], [ -45.0, 40.0 ], [ -45.0, 50.0 ], [-55.0, 50.0 ], [ -55.0, 40.0 ] ]
    ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with two holes');




// 4.2.1.1.6 complex polygon (edges cross)
poly = {
    type: 'Polygon',
    coordinates: [ [
            [ 10.0, 10.0 ],
            [ 20.0, 10.0 ],
            [ 10.0, 20.0 ],
            [ 20.0, 20.0 ],
            [ 10.0, 10.0 ]
    ] ],
    crs: goodCRS
};
curs = coll.find({geo: {$geoWithin: {$geometry: poly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with two holes');



// 4.2.1.1.7 closed polygon (3, 4, 5, 6-sided)
var polys = [
{
    type: 'Polygon',
    coordinates: [ [
            [ 10.0, 10.0 ],
            [ 20.0, 10.0 ],
            [ 15.0, 17.0 ],
            [ 10.0, 10.0 ]
    ] ],
    crs: goodCRS,
    nW: 0, nI: 1
},
{
    type: 'Polygon',
    coordinates: [ [
            [ 10.0, 10.0 ],
            [ 20.0, 10.0 ],
            [ 20.0, 20.0 ],
            [ 10.0, 20.0 ],
            [ 10.0, 10.0 ]
    ] ],
    crs: goodCRS,
    nW: 0, nI: 1
},
{
    type: 'Polygon',  // pentagon
    coordinates: [ [
            [ 10.0, 10.0 ],
            [ 20.0, 10.0 ],
            [ 25.0, 18.0 ],
            [ 15.0, 25.0 ],
            [ 5.0,  18.0 ],
            [ 10.0, 10.0 ]
    ] ],
    crs: goodCRS,
    nW: 0, nI: 1
},
{
    type: 'Polygon',
    coordinates: [ [
            [ 10.0, 10.0 ],
            [ 15.0, 10.0 ],
            [ 22.0, 15.0 ],
            [ 15.0, 20.0 ],
            [ 10.0, 20.0 ],
            [ 7.0,  15.0 ],
            [ 10.0, 10.0 ]
    ] ],
    crs: goodCRS,
    nW: 0, nI: 1
}
];

polys.forEach(
    function(p){
        // first search for within, matching counts against nW
        curs = coll.find({geo: {$geoWithin: {$geometry: p}}});
        assert.eq(curs.count(), p['nW']);
        // search for intersection, counts in nI
        curs = coll.find({geo: {$geoIntersects: {$geometry: p}}});
        assert.eq(curs.count(), p['nI']);
    }
)



// 4.2.1.1.8 closed polygon (101, 1001, 25000-sided)
// compute points on a circle centered at 0,0
// with diameter = D
// and lat*lat + lon*lon = (D/2)*(D/2)
// lat range is -10 to +10
// lon = sqrt( (D/2)*(D/2) - lat*lat )
// and N = number of points
var D = 20;
var N = 101;
var eps = D / Math.ceil((N+1)/2)
print(N,"-sided polygon GEO points");
var lat=0;
var lon=0;
var pts = [];
var i = 0;
var j = N;
// produce longitude values in pairs
// and toss out last point if an odd number requested
for (lat=-(D/2); lat < (D/2); lat+=eps) {
    lon = Math.sqrt( (D/2)*(D/2) - lat*lat );
    print('i:', i)
    pts[i++] = {lat: lat, lon: lon};
    pts[--j] = {lat: lat, lon: -lon};
    print('j:', j)
    print('lat:', lat)
    print('lon:', lon)
}
// ensure we connect the dots
assert(tojson(pts[0]) == tojson(pts[N-1]));







