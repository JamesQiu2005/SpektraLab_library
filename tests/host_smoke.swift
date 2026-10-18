// host_smoke.swift -- the app's side of the bundled helper, in miniature.
//
//   swift tests/host_smoke.swift build/spklib-host "<catalog>.cocatalog"
//
// Starts spklib-host as a child process (as SpektraLab would from
// Contents/Helpers), speaks docs/PROTOCOL.md over its pipes, links the catalog
// into a throwaway library and reads it back. Exit status 0 when every reply
// is what it should be.

import Foundation

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: host_smoke.swift <spklib-host> <catalog>\n".data(using: .utf8)!)
    exit(2)
}

final class Host {
    let process = Process()
    let input = Pipe(), output = Pipe()
    var buffer = Data()
    var nextID = 1

    init(executable: String) throws {
        process.executableURL = URL(fileURLWithPath: executable)
        process.standardInput = input
        process.standardOutput = output
        try process.run()
        _ = try readLine()  // {"event":"ready", ...}
    }

    func readLine() throws -> [String: Any] {
        while true {
            if let nl = buffer.firstIndex(of: 0x0A) {
                let line = buffer[buffer.startIndex..<nl]
                buffer.removeSubrange(buffer.startIndex...nl)
                return try JSONSerialization.jsonObject(with: line) as! [String: Any]
            }
            let chunk = output.fileHandleForReading.availableData
            if chunk.isEmpty { throw NSError(domain: "host closed", code: 1) }
            buffer.append(chunk)
        }
    }

    func call(_ method: String, _ params: [String: Any] = [:]) throws -> [String: Any] {
        let id = nextID
        nextID += 1
        let req: [String: Any] = ["id": id, "method": method, "params": params]
        var data = try JSONSerialization.data(withJSONObject: req)
        data.append(0x0A)
        input.fileHandleForWriting.write(data)
        let reply = try readLine()
        precondition(reply["id"] as? Int == id, "reply out of order")
        if let err = reply["error"] as? [String: Any] { throw NSError(domain: "\(err["code"]!): \(err["message"]!)", code: 2) }
        return reply["result"] as! [String: Any]
    }
}

var failures = 0
func expect(_ ok: Bool, _ what: String) {
    print(ok ? "ok   \(what)" : "FAIL \(what)")
    if !ok { failures += 1 }
}

let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("spklib-swift-\(getpid())")
defer { try? FileManager.default.removeItem(at: tmp) }

do {
    let host = try Host(executable: args[1])
    let started = Date()
    let link = try host.call("capture_one.link", ["source": args[2], "path": tmp.appendingPathComponent("Mirror").path])
    let lib = link["library"] as! String
    let sync = link["sync"] as! [String: Any]
    let images = (sync["images"] as! [String: Any])["added"] as! Int
    expect(images > 0, "linked \(images) images in \(String(format: "%.2f", Date().timeIntervalSince(started))) s")

    let cols = try host.call("collections.list", ["library": lib])["collections"] as! [[String: Any]]
    expect(cols.contains { $0["kind"] as? String == "root" }, "collections: \(cols.count), one root")

    let page = try host.call("images.list", ["library": lib, "limit": 50, "check_files": true, "sort": "captured", "descending": true])
    let list = page["images"] as! [[String: Any]]
    expect(list.count == min(50, page["total"] as! Int), "a page of \(list.count) of \(page["total"]!) images")
    let first = list[0]
    expect((first["path"] as! String).hasPrefix("/"), "newest: \(first["path"]!)")

    // An edit as the app would write it: today's sidecar JSON, verbatim.
    let variant = (first["variants"] as! [[String: Any]])[0]["uuid"] as! String
    let sidecar: [String: Any] = ["schemaVersion": 3, "decoder": "coreimage", "params": ["film_stock": "kodak_portra_400"]]
    let put = try host.call("edit.put", ["library": lib, "variant": variant, "base_revision": 0, "edit": sidecar])
    expect(put["revision"] as? Int == 1, "edit.put -> revision 1")
    let got = try host.call("edit.get", ["library": lib, "variant": variant])
    let film = ((got["edit"] as! [String: Any])["params"] as! [String: Any])["film_stock"] as? String
    expect(film == "kodak_portra_400", "edit.get returns the app's JSON")

    do {
        _ = try host.call("edit.put", ["library": lib, "variant": variant, "base_revision": 0, "edit": sidecar])
        expect(false, "a stale revision is refused")
    } catch {
        expect("\(error)".contains("conflict"), "a stale revision is refused (conflict)")
    }
    let ping = try host.call("host.ping")
    expect(ping["pong"] as? Bool == true, "ping")
    _ = try host.call("host.shutdown")
    host.process.waitUntilExit()
    expect(host.process.terminationStatus == 0, "host exits 0")
} catch {
    print("FAIL \(error)")
    failures += 1
}
exit(failures == 0 ? 0 : 1)
