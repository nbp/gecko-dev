function foo() {
  window.dispatchEvent(new Event("ping"));
}
foo(); // ping (=1)

window.addEventListener("load", function () {
  foo(); // ping (=2)

  // Append a script which should call |foo|, before the encoding of this script
  // bytecode.
  var script = document.createElement("script");
  script.type = "text/javascript";
  script.innerText = "foo();"; // ping (=3)
  document.head.appendChild(script);
});
