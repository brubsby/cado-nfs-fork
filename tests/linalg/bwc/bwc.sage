import re

from bwc_sage import *

if __name__ == '__main__':
    args=dict()
    for v in sys.argv[1:]:
        if (m := re.match(r"^(p)(?:rime)?=(\d+)$", v)):
            args[m.groups()[0]] = Integers()(m.groups()[1])
        elif  (m := re.match(r"^(m|n|nh|nv)=(\d+)$", v)):
            args[m.groups()[0]] = int(m.groups()[1])
        elif  (m := re.match(r"^(wdir|matrix)=(.*)$", v)):
            args[m.groups()[0]] = m.groups()[1]
        else:
            raise ValueError(f"Unknown parameter {v}")
    if "wdir" not in args:
        args["wdir"] = os.path.dirname(args["matrix"])

    def filter_dict(D, pat):
        return dict([kv for kv in D.items() if re.match(pat, kv[0])])

    par = BwcParameters(**filter_dict(args, r"^[mnp]$"))

    M = BwcMatrix(par, **filter_dict(args, r"^(matrix|wdir)$"))
    M.read(force_square=True)
    M.fetch_balancing(**filter_dict(args, r"^n[hv]$"))
    M.check()

    Vs = scan_for_bwc_vectors(par, args["wdir"], read=True)

    MQ = M.decorrelated()

    check_all_vectors(Vs, MQ)

    c = BwcCheckData(par, M.dimensions(), args["wdir"])
    c.read()
    c.check(MQ)

    a = BwcAFiles(par, M.dimensions(), args["wdir"])
    a.read()
    a.check(MQ)

    print("All checks passed 🥳")
