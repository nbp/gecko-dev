var stateMachineResolve, stateMachineReject;
var statePromise = new Promise((resolve, reject) => {
  stateMachineResolve = resolve;
  stateMachineReject = reject;
});
var stateHistory = [];
var stateMachine = {
  "scriptloader_load_source": {
    "scriptloader_encode_and_execute": {
      "scriptloader_bytecode_saved": "bytecode_saved",
      "scriptloader_bytecode_failed": "bytecode_failed"
    },
    "scriptloader_execute": "source_exec"
  },
  "scriptloader_load_bytecode": {
    "scriptloader_fallback": {
      // Replicate the top-level without "scriptloader_load_bytecode"
      "scriptloader_load_source": {
        "scriptloader_encode_and_execute": {
          "scriptloader_bytecode_saved": "fallback_bytecode_saved",
          "scriptloader_bytecode_failed": "fallback_bytecode_failed"
        },
        "scriptloader_execute": "fallback_source_exec"
      }
    },
    "scriptloader_execute": "bytecode_exec"
  }
};

function log_event(evt) {
  // Suppose that there is only one script tag.
  stateHistory.push(evt.type)
  if (typeof stateMachine == "object")
    stateMachine = stateMachine[evt.type];
  if (typeof stateMachine == "string") {
    // We arrived to a final state, report the name of it.
    stateMachineResolve(stateMachine);
  } else if (stateMachine === undefined) {
    // We followed an unknwon transition, report the full history.
    setTimeout(() => stateMachineReject(stateHistory), 500);
  }
}

function register_promise(resolve, reject) {
  statePromise.then(resolve, reject);
}

var s = document.getElementById("watchme");
s.addEventListener("scriptloader_load_source", log_event);
s.addEventListener("scriptloader_load_bytecode", log_event);
s.addEventListener("scriptloader_generate_bytecode", log_event);
s.addEventListener("scriptloader_execute", log_event);
s.addEventListener("scriptloader_bytecode_saved", log_event);
s.addEventListener("scriptloader_bytecode_failed", log_event);
s.addEventListener("scriptloader_fallback", log_event);
