(function () {
    const payloadPath = __PAYLOAD_PATH__;

    try {
        const module = Module.load(payloadPath);
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
