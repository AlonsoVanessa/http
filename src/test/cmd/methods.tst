/*
 *  methods.tst - Test various http methods
 */

require ejs.test
load("http/support.es")

//  Options/Trace
run("--method OPTIONS /index.html")
assert(run("--showCode -q --method TRACE /index.html").contains("406"))
