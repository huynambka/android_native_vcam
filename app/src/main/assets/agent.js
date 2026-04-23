(function () {
    const payloadPath = __PAYLOAD_PATH__;

    try {
        const module = Module.load(payloadPath);
        const payloadName = payloadPath.split("/").pop();

        if (payloadName === "libhook.so") {
            const entry = module.findExportByName("main_hook");

            if (entry === null) {
                throw new Error("main_hook export not found");
            }

            const mainHook = new NativeFunction(entry, "void", []);
            mainHook();
        }

        send({
            type: "loaded",
            path: module.path,
            base: module.base.toString()
        });
    } catch (e) {
        send({
            type: "error",
            message: String(e),
            stack: e && e.stack ? e.stack : ""
        });
    }
})();
